#include "MotionLink.h"

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>

#include <algorithm>
#include <chrono>

#include "Attestation.h"
#include "RelayLog.h"

#pragma comment(lib, "ws2_32.lib")

namespace
{
    SOCKET AsSocket(uintptr_t p_Value)
    {
        return static_cast<SOCKET>(p_Value);
    }
}

MotionLink::MotionLink(Options p_Options, std::string p_AdapterInstanceId, Clock p_Clock, CommandGeneration p_CommandGeneration) :
    m_Options(std::move(p_Options)),
    m_AdapterInstanceId(std::move(p_AdapterInstanceId)),
    m_Clock(std::move(p_Clock)),
    m_CommandGeneration(std::move(p_CommandGeneration))
{
    // The motion link binds only with the run token (section 15.5): without a valid one it never
    // connects (BEAM would refuse it anyway: the command link is not token-verified).
    if (!Attestation::ValidToken(m_Options.token))
    {
        RelayLog::Warn("motion link: no valid run token (64 lowercase hex); the motion link will not connect");
        m_Options.token.clear();
        return;
    }

    WSADATA s_Data;
    if (WSAStartup(MAKEWORD(2, 2), &s_Data) != 0)
    {
        RelayLog::Error("motion link: WSAStartup failed; the link will never connect");
        return;
    }

    m_WinsockStarted = true;
    m_Thread = std::thread([this] { Loop(); });

    RelayLog::Info(
        "motion link: client to {}:{} (connect timeout {} ms, welcome timeout {} ms, send deadline {} ms, backoff {}..{} ms; inbound {} / control {} / samples {}); binds only to the attested version-2 command link; token present (never logged)",
        m_Options.host, m_Options.port, m_Options.connect_timeout_ms, m_Options.welcome_timeout_ms, m_Options.send_timeout_ms,
        m_Options.backoff_initial_ms, m_Options.backoff_max_ms, k_InboundCapacity, k_ControlCapacity, k_SampleCapacity
    );
}

MotionLink::~MotionLink()
{
    {
        std::lock_guard s_Lock(m_Mutex);
        m_Stop = true;
    }

    m_Wake.notify_all();

    if (m_Thread.joinable())
        m_Thread.join();

    if (m_WinsockStarted)
        WSACleanup();
}

MotionLink::Binding MotionLink::Current() const
{
    std::lock_guard s_Lock(m_Mutex);
    return m_Binding;
}

std::vector<MotionLink::Inbound> MotionLink::Drain()
{
    std::lock_guard s_Lock(m_Mutex);
    std::vector<Inbound> s_Out(m_Inbound.begin(), m_Inbound.end());
    m_Inbound.clear();
    return s_Out;
}

MotionLink::Queued MotionLink::SendControl(uint64_t p_Serial, std::string p_Line)
{
    {
        std::lock_guard s_Lock(m_Mutex);
        if (!m_Binding.bound || m_Binding.serial != p_Serial)
            return Queued::NotBound;
        if (m_Control.size() >= k_ControlCapacity)
        {
            ++m_Stats.control_full;
            return Queued::Full;
        }
        m_Control.push_back(std::move(p_Line));
        ++m_Stats.control_queued;
    }

    m_Wake.notify_one();
    return Queued::Queued;
}

MotionLink::Queued MotionLink::SendSample(uint64_t p_Serial, std::string p_Line)
{
    {
        std::lock_guard s_Lock(m_Mutex);
        if (!m_Binding.bound || m_Binding.serial != p_Serial)
            return Queued::NotBound;
        if (m_Samples.size() >= k_SampleCapacity)
        {
            ++m_Stats.samples_full;
            return Queued::Full;
        }
        m_Samples.push_back(std::move(p_Line));
        ++m_Stats.samples_queued;
    }

    m_Wake.notify_one();
    return Queued::Queued;
}

void MotionLink::Drop(uint64_t p_Serial, const char* p_Reason)
{
    {
        std::lock_guard s_Lock(m_Mutex);
        if (!m_Binding.bound || m_Binding.serial != p_Serial)
            return;
        m_DropSerial = p_Serial;
        m_DropReason = p_Reason;
        ++m_Stats.drop_requests;
    }

    m_Wake.notify_one();
}

MotionLink::Stats MotionLink::GetStats() const
{
    std::lock_guard s_Lock(m_Mutex);
    return m_Stats;
}

bool MotionLink::WaitStop(uint32_t p_TimeoutMs)
{
    std::unique_lock s_Lock(m_Mutex);
    return m_Wake.wait_for(s_Lock, std::chrono::milliseconds(p_TimeoutMs), [this] { return m_Stop; });
}

void MotionLink::Loop()
{
    {
        std::lock_guard s_Lock(m_Mutex);
        m_Stats.network_thread_id = GetCurrentThreadId();
    }

    uint32_t s_Backoff = m_Options.backoff_initial_ms;
    bool s_WaitBeforeConnect = false;
    bool s_LoggedWaiting = false;

    for (;;)
    {
        {
            std::lock_guard s_Lock(m_Mutex);
            if (m_Stop)
                break;
        }

        if (AsSocket(m_Socket) == INVALID_SOCKET)
        {
            // Bind only to an attested version-2 command link; poll for it otherwise.
            const uint64_t s_CommandGeneration = m_CommandGeneration ? m_CommandGeneration() : 0;
            if (s_CommandGeneration == 0)
            {
                if (!s_LoggedWaiting)
                {
                    s_LoggedWaiting = true;
                    RelayLog::Info("motion link: waiting for an attested command link before connecting");
                }
                if (WaitStop(50))
                    break;
                continue;
            }
            s_LoggedWaiting = false;

            if (s_WaitBeforeConnect)
            {
                // Paced like the command link (M5 N4): a refused, lost or failed binding is retried at
                // a bounded rate, always as a new connection with a new welcome.
                s_WaitBeforeConnect = false;
                {
                    std::lock_guard s_Lock(m_Mutex);
                    ++m_Stats.reconnect_waits;
                }
                RelayLog::Warn("motion link: reconnecting to {}:{} in {} ms", m_Options.host, m_Options.port, s_Backoff);
                if (WaitStop(s_Backoff))
                    break;
                s_Backoff = std::min<uint32_t>(s_Backoff * 2, m_Options.backoff_max_ms);
                continue; // re-read the command link before connecting
            }

            if (Connect() && Handshake(s_CommandGeneration))
            {
                s_Backoff = m_Options.backoff_initial_ms;
                continue;
            }

            if (AsSocket(m_Socket) != INVALID_SOCKET)
                Disconnect("binding failed");
            s_WaitBeforeConnect = true;
            continue;
        }

        // Bound: the binding lives only as long as the command link generation it named, and as
        // long as the frame thread can deliver its evidence.
        Binding s_Binding;
        uint64_t s_DropSerial;
        std::string s_DropReason;
        {
            std::lock_guard s_Lock(m_Mutex);
            s_Binding = m_Binding;
            s_DropSerial = m_DropSerial;
            s_DropReason = m_DropReason;
        }

        const uint64_t s_CommandNow = m_CommandGeneration ? m_CommandGeneration() : 0;
        if (s_CommandNow != s_Binding.command_generation)
        {
            {
                std::lock_guard s_Lock(m_Mutex);
                ++m_Stats.command_link_changes;
            }
            Disconnect("the command link it was bound to changed or was lost");
            s_WaitBeforeConnect = true;
            continue;
        }

        if (s_DropSerial != 0 && s_DropSerial == s_Binding.serial)
        {
            Disconnect(s_DropReason.empty() ? "dropped by the frame thread" : s_DropReason.c_str());
            s_WaitBeforeConnect = true;
            continue;
        }

        if (!Flush() || !ReadLines(m_Options.poll_ms) || !Flush())
        {
            s_WaitBeforeConnect = true;
            continue;
        }
    }

    Disconnect("link shutting down");
}

bool MotionLink::Connect()
{
    {
        std::lock_guard s_Lock(m_Mutex);
        ++m_Stats.connect_attempts;
    }

    sockaddr_in s_Address = {};
    s_Address.sin_family = AF_INET;
    s_Address.sin_port = htons(m_Options.port);

    if (m_Options.host != "127.0.0.1" || inet_pton(AF_INET, m_Options.host.c_str(), &s_Address.sin_addr) != 1)
    {
        RelayLog::Error("motion link: '{}' is not the loopback address; giving up", m_Options.host);
        std::lock_guard s_Lock(m_Mutex);
        m_Stop = true;
        return false;
    }

    const SOCKET s_Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_Socket == INVALID_SOCKET)
        return false;

    if (m_Options.send_buffer_bytes > 0)
        setsockopt(s_Socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&m_Options.send_buffer_bytes), sizeof(m_Options.send_buffer_bytes));

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

    // The send deadline (section 15.6): a write that cannot complete within it fails, and the link
    // is dropped (Windows leaves the socket in an indeterminate state after a send timeout).
    const DWORD s_SendTimeout = m_Options.send_timeout_ms;
    setsockopt(s_Socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&s_SendTimeout), sizeof(s_SendTimeout));
    const BOOL s_NoDelay = TRUE;
    setsockopt(s_Socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&s_NoDelay), sizeof(s_NoDelay));

    m_Socket = static_cast<uintptr_t>(s_Socket);
    m_Buffer.clear();

    {
        std::lock_guard s_Lock(m_Mutex);
        m_Stats.connected = true;
        ++m_Stats.connects;
    }

    RelayLog::Info("motion link: connected to {}:{}", m_Options.host, m_Options.port);
    return true;
}

// motion.hello, then motion.welcome (or motion.refused) within the welcome timeout, then one
// motion.attest over that welcome's nonce and generation. BEAM gives a good attest no reply.
bool MotionLink::Handshake(uint64_t p_CommandGeneration)
{
    if (!SendLine(MotionProtocol::EncodeHello(m_AdapterInstanceId, p_CommandGeneration)))
        return false;

    const uint64_t s_Deadline = m_Clock() + m_Options.welcome_timeout_ms;

    while (m_Clock() < s_Deadline)
    {
        {
            std::lock_guard s_Lock(m_Mutex);
            if (m_Stop)
                return false;
        }

        fd_set s_Readable;
        FD_ZERO(&s_Readable);
        FD_SET(AsSocket(m_Socket), &s_Readable);
        timeval s_Timeout = {0, 20000};
        if (select(0, &s_Readable, nullptr, nullptr, &s_Timeout) <= 0)
            continue;

        char s_Chunk[1024];
        const int s_Received = recv(AsSocket(m_Socket), s_Chunk, sizeof(s_Chunk), 0);
        if (s_Received <= 0)
        {
            RelayLog::Warn("motion link: BEAM closed the connection during the binding");
            return false;
        }

        m_Buffer.append(s_Chunk, static_cast<size_t>(s_Received));

        const size_t s_Newline = m_Buffer.find('\n');
        if (s_Newline == std::string::npos)
        {
            if (m_Buffer.size() > MotionProtocol::k_MaxLineBytes)
            {
                std::lock_guard s_Lock(m_Mutex);
                ++m_Stats.violations;
                RelayLog::Warn("motion link: welcome line exceeds {} bytes", MotionProtocol::k_MaxLineBytes);
                return false;
            }
            continue;
        }

        const std::string s_Line = m_Buffer.substr(0, s_Newline);
        m_Buffer.erase(0, s_Newline + 1);

        const auto s_Decoded = MotionProtocol::DecodeFromBeam(s_Line);
        if (!s_Decoded.ok)
        {
            std::lock_guard s_Lock(m_Mutex);
            ++m_Stats.violations;
            RelayLog::Warn("motion link: invalid line during the binding ({})", s_Decoded.error);
            return false;
        }

        if (s_Decoded.message.kind == MotionProtocol::Kind::Refused)
        {
            std::lock_guard s_Lock(m_Mutex);
            ++m_Stats.refusals;
            m_Stats.last_refusal = s_Decoded.message.reason;
            RelayLog::Warn("motion link: refused by BEAM ({})", s_Decoded.message.reason);
            return false;
        }

        if (s_Decoded.message.kind != MotionProtocol::Kind::Welcome)
        {
            std::lock_guard s_Lock(m_Mutex);
            ++m_Stats.violations;
            RelayLog::Warn("motion link: {} before the binding; closing", MotionProtocol::KindText(s_Decoded.message.kind));
            return false;
        }

        // The command link must still be the one the hello named (it may have changed meanwhile).
        if ((m_CommandGeneration ? m_CommandGeneration() : 0) != p_CommandGeneration)
        {
            RelayLog::Warn("motion link: the command link changed during the binding; closing");
            return false;
        }

        const uint64_t s_MotionGeneration = s_Decoded.message.motion_link_generation;
        const std::string s_Mac = Attestation::MotionAttestMac(m_Options.token, s_Decoded.message.nonce, s_MotionGeneration, p_CommandGeneration, m_AdapterInstanceId);

        {
            std::lock_guard s_Lock(m_Mutex);
            ++m_Stats.welcomes;
        }

        if (!SendLine(MotionProtocol::EncodeAttest(s_MotionGeneration, p_CommandGeneration, m_AdapterInstanceId, s_Mac)))
            return false;

        {
            std::lock_guard s_Lock(m_Mutex);
            ++m_Stats.attests_sent;
            m_Binding.serial += 1;
            m_Binding.bound = true;
            m_Binding.motion_generation = s_MotionGeneration;
            m_Binding.command_generation = p_CommandGeneration;
            m_DropSerial = 0;
            m_DropReason.clear();
            m_Stats.bound = true;
            m_Stats.serial = m_Binding.serial;
            m_Stats.motion_generation = s_MotionGeneration;
            m_Stats.command_generation = p_CommandGeneration;
        }

        RelayLog::Info("motion link: attest sent; bound as motion link generation {} to command link generation {} (serial {}; nonce and MAC not logged)", s_MotionGeneration, p_CommandGeneration, m_Binding.serial);
        return true;
    }

    RelayLog::Warn("motion link: no welcome within {} ms", m_Options.welcome_timeout_ms);
    return false;
}

void MotionLink::Disconnect(const char* p_Reason)
{
    if (AsSocket(m_Socket) == INVALID_SOCKET)
        return;

    closesocket(AsSocket(m_Socket));
    m_Socket = ~uintptr_t(0);
    m_Buffer.clear();

    size_t s_Discarded;
    bool s_WasBound;
    uint64_t s_Serial;
    {
        std::lock_guard s_Lock(m_Mutex);
        s_WasBound = m_Binding.bound;
        s_Serial = m_Binding.serial;
        m_Binding.bound = false;
        s_Discarded = m_Control.size() + m_Samples.size();
        m_Control.clear();
        m_Samples.clear();
        m_DropSerial = 0;
        m_Stats.connected = false;
        m_Stats.bound = false;
        ++m_Stats.disconnects;
        m_Stats.discarded_on_disconnect += s_Discarded;
        m_Stats.last_disconnect = p_Reason;
        // The frame thread notices the end of the binding before any further setter call.
        m_Signals.fetch_add(1);
    }

    RelayLog::Warn("motion link: disconnected ({}); {} unsent line(s) discarded{}", p_Reason, s_Discarded, s_WasBound ? fmt::format("; binding serial {} ended", s_Serial) : std::string());
}

// Reads what is available within p_WaitMs and queues every complete line's message. False when
// the connection ended (it is then closed).
bool MotionLink::ReadLines(uint32_t p_WaitMs)
{
    fd_set s_Readable;
    FD_ZERO(&s_Readable);
    FD_SET(AsSocket(m_Socket), &s_Readable);
    timeval s_Timeout = {static_cast<long>(p_WaitMs / 1000), static_cast<long>((p_WaitMs % 1000) * 1000)};

    const int s_Ready = select(0, &s_Readable, nullptr, nullptr, &s_Timeout);
    if (s_Ready < 0)
    {
        Disconnect("select failed");
        return false;
    }
    if (s_Ready == 0)
        return true;

    char s_Chunk[2048];
    const int s_Received = recv(AsSocket(m_Socket), s_Chunk, sizeof(s_Chunk), 0);
    if (s_Received == 0)
    {
        Disconnect("BEAM closed the connection");
        return false;
    }
    if (s_Received < 0)
    {
        Disconnect("recv failed");
        return false;
    }

    m_Buffer.append(s_Chunk, static_cast<size_t>(s_Received));

    for (;;)
    {
        const size_t s_Newline = m_Buffer.find('\n');
        if (s_Newline == std::string::npos)
        {
            if (m_Buffer.size() > MotionProtocol::k_MaxLineBytes)
            {
                {
                    std::lock_guard s_Lock(m_Mutex);
                    ++m_Stats.violations;
                }
                Disconnect("a partial line exceeds 1,024 bytes");
                return false;
            }
            return true;
        }

        const std::string s_Line = m_Buffer.substr(0, s_Newline);
        m_Buffer.erase(0, s_Newline + 1);

        if (!HandleLine(s_Line))
            return false;
    }
}

// One line after the binding: a target, a close or a share close; anything else (a second welcome,
// a refusal, an undecodable or oversized line) closes the link.
bool MotionLink::HandleLine(std::string_view p_Line)
{
    const auto s_Decoded = MotionProtocol::DecodeFromBeam(p_Line);
    const uint64_t s_Now = m_Clock();

    std::string s_Violation;
    {
        std::lock_guard s_Lock(m_Mutex);
        ++m_Stats.lines_received;

        if (!s_Decoded.ok)
        {
            s_Violation = s_Decoded.error;
        }
        else if (s_Decoded.message.kind == MotionProtocol::Kind::Refused)
        {
            ++m_Stats.refusals;
            m_Stats.last_refusal = s_Decoded.message.reason;
            s_Violation = "motion.refused after the binding: " + s_Decoded.message.reason;
        }
        else if (s_Decoded.message.kind == MotionProtocol::Kind::Welcome)
        {
            s_Violation = "a second motion.welcome";
        }
        else if (m_Inbound.size() >= k_InboundCapacity)
        {
            s_Violation = "inbound queue full (a flood is never legitimate: BEAM keeps one target in flight)";
        }
        else
        {
            if (s_Decoded.message.kind == MotionProtocol::Kind::Target)
                ++m_Stats.targets_received;
            else if (s_Decoded.message.kind == MotionProtocol::Kind::Close)
                ++m_Stats.closes_received;
            else
                ++m_Stats.share_closes_received;
            m_Inbound.push_back(Inbound{s_Decoded.message, s_Now, m_Binding.serial});
            // A close request is a cancellation the frame thread re-checks immediately before a
            // setter call; signalled under the lock that queued it.
            if (s_Decoded.message.kind != MotionProtocol::Kind::Target)
                m_Signals.fetch_add(1);
        }

        if (!s_Violation.empty())
            ++m_Stats.violations;
    }

    if (!s_Violation.empty())
    {
        RelayLog::Warn("motion link: protocol violation ({}); closing", s_Violation);
        Disconnect("protocol violation");
        return false;
    }

    return true;
}

// Writes every queued control line, then every queued sample, each within the send deadline.
bool MotionLink::Flush()
{
    for (;;)
    {
        std::string s_Line;
        {
            std::lock_guard s_Lock(m_Mutex);
            if (!m_Control.empty())
            {
                s_Line = std::move(m_Control.front());
                m_Control.pop_front();
            }
            else if (!m_Samples.empty())
            {
                s_Line = std::move(m_Samples.front());
                m_Samples.pop_front();
            }
            else
            {
                return true;
            }
        }

        if (!SendLine(s_Line))
            return false;
    }
}

bool MotionLink::SendLine(const std::string& p_Line)
{
    size_t s_Offset = 0;

    while (s_Offset < p_Line.size())
    {
        const int s_Sent = send(AsSocket(m_Socket), p_Line.data() + s_Offset, static_cast<int>(p_Line.size() - s_Offset), 0);
        if (s_Sent == SOCKET_ERROR)
        {
            const int s_Error = WSAGetLastError();
            {
                std::lock_guard s_Lock(m_Mutex);
                ++m_Stats.send_failures;
            }
            RelayLog::Warn("motion link: send failed with error {}{}", s_Error, s_Error == WSAETIMEDOUT ? " (the send deadline passed)" : "");
            Disconnect(s_Error == WSAETIMEDOUT ? "send deadline passed" : "send failed");
            return false;
        }
        s_Offset += static_cast<size_t>(s_Sent);
    }

    std::lock_guard s_Lock(m_Mutex);
    ++m_Stats.lines_sent;
    return true;
}
