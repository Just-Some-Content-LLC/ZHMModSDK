#include "TcpRelaySink.h"

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <algorithm>
#include <chrono>

#include "RelayLog.h"

#pragma comment(lib, "ws2_32.lib")

namespace
{
    SOCKET AsSocket(uintptr_t p_Value)
    {
        return static_cast<SOCKET>(p_Value);
    }
}

TcpRelaySink::TcpRelaySink(Options p_Options) :
    m_Options(std::move(p_Options))
{
    WSADATA s_Data;
    const int s_Result = WSAStartup(MAKEWORD(2, 2), &s_Data);

    if (s_Result != 0)
    {
        RelayLog::Error("tcp sink: WSAStartup failed with {}; the sink will drop everything", s_Result);
        return;
    }

    m_WinsockStarted = true;
    m_Thread = std::thread([this] { SenderLoop(); });

    RelayLog::Info(
        "tcp sink: client to {}:{} (queue {}, connect timeout {} ms, backoff {}..{} ms)",
        m_Options.host, m_Options.port, m_Options.max_queued, m_Options.connect_timeout_ms,
        m_Options.backoff_initial_ms, m_Options.backoff_max_ms
    );
}

TcpRelaySink::~TcpRelaySink()
{
    {
        std::lock_guard s_Lock(m_Mutex);
        m_Stop = true;
    }

    m_Wake.notify_all();

    // The loop checks m_Stop between short waits, so this returns within a few seconds at most.
    if (m_Thread.joinable())
        m_Thread.join();

    if (m_WinsockStarted)
        WSACleanup();
}

void TcpRelaySink::Publish(const PublishedEnvelope& p_Envelope)
{
    std::string s_Line = p_Envelope.json;
    s_Line.push_back('\n');

    bool s_DroppedOldest = false;

    {
        std::lock_guard s_Lock(m_Mutex);
        ++m_Stats.enqueued;

        if (!m_WinsockStarted || !m_Stats.connected)
        {
            // Best-effort only: nothing is queued for a connection that does not exist yet.
            ++m_Stats.dropped;
            RelayLog::Warn("tcp sink: not connected; dropped {} #{}", p_Envelope.event_type, p_Envelope.sequence);
            return;
        }

        if (m_Queue.size() >= m_Options.max_queued)
        {
            m_Queue.pop_front();
            ++m_Stats.dropped;
            s_DroppedOldest = true;
        }

        m_Queue.push_back({p_Envelope.sequence, std::move(s_Line)});
    }

    if (s_DroppedOldest)
        RelayLog::Warn("tcp sink: queue full; dropped the oldest queued envelope");

    RelayLog::Info("tcp sink: queued {} #{}", p_Envelope.event_type, p_Envelope.sequence);
    m_Wake.notify_one();
}

TcpRelaySink::Stats TcpRelaySink::GetStats() const
{
    std::lock_guard s_Lock(m_Mutex);
    return m_Stats;
}

// Returns true when there is a queued line or a stop request; false on timeout.
bool TcpRelaySink::WaitForWork(uint32_t p_TimeoutMs)
{
    std::unique_lock s_Lock(m_Mutex);
    return m_Wake.wait_for(s_Lock, std::chrono::milliseconds(p_TimeoutMs), [this] { return m_Stop || !m_Queue.empty(); });
}

void TcpRelaySink::SenderLoop()
{
    uint32_t s_Backoff = m_Options.backoff_initial_ms;
    uint64_t s_FailuresSinceLog = 0;

    for (;;)
    {
        {
            std::lock_guard s_Lock(m_Mutex);
            if (m_Stop)
                break;
        }

        if (AsSocket(m_Socket) == INVALID_SOCKET)
        {
            if (TryConnect())
            {
                s_Backoff = m_Options.backoff_initial_ms;
                s_FailuresSinceLog = 0;
                continue;
            }

            // Log the first failure and then once per backoff doubling, so an absent backend
            // produces a handful of lines, not one per second.
            if (s_FailuresSinceLog == 0)
                RelayLog::Warn("tcp sink: backend not reachable at {}:{}; retrying in {} ms", m_Options.host, m_Options.port, s_Backoff);

            ++s_FailuresSinceLog;

            WaitForWork(s_Backoff); // a stop request interrupts the wait; queued lines cannot exist while disconnected

            if (s_Backoff < m_Options.backoff_max_ms)
            {
                s_Backoff = std::min<uint32_t>(s_Backoff * 2, m_Options.backoff_max_ms);
                s_FailuresSinceLog = 0;
            }

            continue;
        }

        // Connected: wait for a line, or wake periodically to notice a closed peer.
        WaitForWork(500);

        Queued s_Item;

        {
            std::lock_guard s_Lock(m_Mutex);

            if (m_Stop)
                break;

            if (!m_Queue.empty())
            {
                s_Item = std::move(m_Queue.front());
                m_Queue.pop_front();
            }
        }

        DiscardInbound();

        if (AsSocket(m_Socket) == INVALID_SOCKET)
        {
            if (!s_Item.line.empty())
            {
                std::lock_guard s_Lock(m_Mutex);
                ++m_Stats.dropped;
                RelayLog::Warn("tcp sink: connection lost before #{} was sent; dropped", s_Item.sequence);
            }

            continue;
        }

        if (s_Item.line.empty())
            continue;

        if (SendLine(s_Item.line))
        {
            {
                std::lock_guard s_Lock(m_Mutex);
                ++m_Stats.sent;
            }

            RelayLog::Info("tcp sink: sent #{} ({} bytes)", s_Item.sequence, s_Item.line.size());
        }
        else
        {
            std::lock_guard s_Lock(m_Mutex);
            ++m_Stats.dropped;
            RelayLog::Warn("tcp sink: #{} not delivered", s_Item.sequence);
        }
    }

    Disconnect("sink shutting down");
}

bool TcpRelaySink::TryConnect()
{
    {
        std::lock_guard s_Lock(m_Mutex);
        ++m_Stats.connect_attempts;
    }

    sockaddr_in s_Address = {};
    s_Address.sin_family = AF_INET;
    s_Address.sin_port = htons(m_Options.port);

    if (inet_pton(AF_INET, m_Options.host.c_str(), &s_Address.sin_addr) != 1)
    {
        RelayLog::Error("tcp sink: '{}' is not an IPv4 address; giving up", m_Options.host);
        std::lock_guard s_Lock(m_Mutex);
        m_Stop = true;
        return false;
    }

    const SOCKET s_Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (s_Socket == INVALID_SOCKET)
        return false;

    u_long s_NonBlocking = 1;
    ioctlsocket(s_Socket, FIONBIO, &s_NonBlocking);

    bool s_Connected = false;

    if (connect(s_Socket, reinterpret_cast<sockaddr*>(&s_Address), sizeof(s_Address)) == 0)
    {
        s_Connected = true;
    }
    else if (WSAGetLastError() == WSAEWOULDBLOCK)
    {
        fd_set s_Writable, s_Failed;
        FD_ZERO(&s_Writable);
        FD_ZERO(&s_Failed);
        FD_SET(s_Socket, &s_Writable);
        FD_SET(s_Socket, &s_Failed);

        timeval s_Timeout = {};
        s_Timeout.tv_sec = m_Options.connect_timeout_ms / 1000;
        s_Timeout.tv_usec = (m_Options.connect_timeout_ms % 1000) * 1000;

        if (select(0, nullptr, &s_Writable, &s_Failed, &s_Timeout) > 0 && FD_ISSET(s_Socket, &s_Writable))
        {
            int s_Error = 0;
            int s_Length = sizeof(s_Error);
            getsockopt(s_Socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&s_Error), &s_Length);
            s_Connected = s_Error == 0;
        }
    }

    if (!s_Connected)
    {
        closesocket(s_Socket);
        return false;
    }

    s_NonBlocking = 0;
    ioctlsocket(s_Socket, FIONBIO, &s_NonBlocking);

    const DWORD s_SendTimeout = m_Options.send_timeout_ms;
    setsockopt(s_Socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&s_SendTimeout), sizeof(s_SendTimeout));

    const BOOL s_NoDelay = TRUE;
    setsockopt(s_Socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&s_NoDelay), sizeof(s_NoDelay));

    m_Socket = static_cast<uintptr_t>(s_Socket);

    {
        std::lock_guard s_Lock(m_Mutex);
        m_Stats.connected = true;
        ++m_Stats.connects;
    }

    RelayLog::Info("tcp sink: connected to {}:{}", m_Options.host, m_Options.port);
    return true;
}

void TcpRelaySink::Disconnect(const char* p_Reason)
{
    if (AsSocket(m_Socket) == INVALID_SOCKET)
        return;

    closesocket(AsSocket(m_Socket));
    m_Socket = ~uintptr_t(0);

    size_t s_Discarded;

    {
        std::lock_guard s_Lock(m_Mutex);
        m_Stats.connected = false;
        ++m_Stats.disconnects;
        s_Discarded = m_Queue.size();
        m_Stats.dropped += s_Discarded;
        m_Queue.clear();
    }

    RelayLog::Warn("tcp sink: disconnected ({}); {} queued envelope(s) discarded", p_Reason, s_Discarded);
}

bool TcpRelaySink::SendLine(const std::string& p_Line)
{
    size_t s_Offset = 0;

    while (s_Offset < p_Line.size())
    {
        const int s_Sent = send(AsSocket(m_Socket), p_Line.data() + s_Offset, static_cast<int>(p_Line.size() - s_Offset), 0);

        if (s_Sent == SOCKET_ERROR)
        {
            const int s_Error = WSAGetLastError();
            RelayLog::Warn("tcp sink: send failed with error {}", s_Error);
            Disconnect("send failed");
            return false;
        }

        s_Offset += static_cast<size_t>(s_Sent);
    }

    return true;
}

// There is no inbound protocol. Anything the peer sends is read and thrown away, and a closed
// peer is noticed here.
void TcpRelaySink::DiscardInbound()
{
    if (AsSocket(m_Socket) == INVALID_SOCKET)
        return;

    u_long s_NonBlocking = 1;
    ioctlsocket(AsSocket(m_Socket), FIONBIO, &s_NonBlocking);

    char s_Buffer[512];
    uint64_t s_Discarded = 0;
    bool s_Closed = false;

    for (;;)
    {
        const int s_Received = recv(AsSocket(m_Socket), s_Buffer, sizeof(s_Buffer), 0);

        if (s_Received > 0)
        {
            s_Discarded += static_cast<uint64_t>(s_Received);
            continue;
        }

        if (s_Received == 0)
            s_Closed = true;
        else if (WSAGetLastError() != WSAEWOULDBLOCK)
            s_Closed = true;

        break;
    }

    s_NonBlocking = 0;
    ioctlsocket(AsSocket(m_Socket), FIONBIO, &s_NonBlocking);

    if (s_Discarded > 0)
    {
        bool s_First;

        {
            std::lock_guard s_Lock(m_Mutex);
            s_First = m_Stats.inbound_bytes_discarded == 0;
            m_Stats.inbound_bytes_discarded += s_Discarded;
        }

        if (s_First)
            RelayLog::Warn("tcp sink: the backend sent {} byte(s); there is no inbound protocol, discarded", s_Discarded);
    }

    if (s_Closed)
        Disconnect("peer closed");
}
