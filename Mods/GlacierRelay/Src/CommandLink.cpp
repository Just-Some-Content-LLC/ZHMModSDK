#include "CommandLink.h"

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>

#include <algorithm>
#include <chrono>

#include "RelayLog.h"

#pragma comment(lib, "ws2_32.lib")

using CommandProtocol::Result;
using CommandProtocol::Status;

namespace
{
    SOCKET AsSocket(uintptr_t p_Value)
    {
        return static_cast<SOCKET>(p_Value);
    }

    std::string UtcNowText()
    {
        SYSTEMTIME s_Now;
        GetSystemTime(&s_Now);
        char s_Buffer[32];
        snprintf(s_Buffer, sizeof(s_Buffer), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", s_Now.wYear, s_Now.wMonth, s_Now.wDay, s_Now.wHour, s_Now.wMinute, s_Now.wSecond, s_Now.wMilliseconds);
        return s_Buffer;
    }
}

CommandLink::CommandLink(Options p_Options, CommandQueue& p_Queue, std::string p_AdapterInstanceId, Clock p_Clock) :
    m_Options(std::move(p_Options)),
    m_Queue(p_Queue),
    m_AdapterInstanceId(std::move(p_AdapterInstanceId)),
    m_Clock(std::move(p_Clock))
{
    WSADATA s_Data;

    if (WSAStartup(MAKEWORD(2, 2), &s_Data) != 0)
    {
        RelayLog::Error("command link: WSAStartup failed; the link will never connect");
        return;
    }

    m_WinsockStarted = true;
    m_Thread = std::thread([this] { Loop(); });

    RelayLog::Info(
        "command link: client to {}:{} (connect timeout {} ms, welcome timeout {} ms, backoff {}..{} ms, queue {}, dedup window {})",
        m_Options.host, m_Options.port, m_Options.connect_timeout_ms, m_Options.welcome_timeout_ms,
        m_Options.backoff_initial_ms, m_Options.backoff_max_ms, CommandQueue::k_Capacity, CommandQueue::k_DedupWindow
    );
}

CommandLink::~CommandLink()
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

void CommandLink::PushResult(const Result& p_Result)
{
    {
        std::lock_guard s_Lock(m_Mutex);
        m_Outgoing.push_back(CommandProtocol::EncodeResult(p_Result));
    }

    m_Wake.notify_one();
}

void CommandLink::SetMissionGeneration(const std::optional<std::string>& p_Generation)
{
    std::lock_guard s_Lock(m_Mutex);
    m_MissionGeneration = p_Generation;
}

uint64_t CommandLink::LinkGeneration() const
{
    std::lock_guard s_Lock(m_Mutex);
    return m_Stats.welcomed ? m_Stats.link_generation : 0;
}

CommandLink::Stats CommandLink::GetStats() const
{
    std::lock_guard s_Lock(m_Mutex);
    return m_Stats;
}

bool CommandLink::WaitStop(uint32_t p_TimeoutMs)
{
    std::unique_lock s_Lock(m_Mutex);
    return m_Wake.wait_for(s_Lock, std::chrono::milliseconds(p_TimeoutMs), [this] { return m_Stop; });
}

void CommandLink::Loop()
{
    {
        std::lock_guard s_Lock(m_Mutex);
        m_Stats.network_thread_id = GetCurrentThreadId();
    }

    uint32_t s_Backoff = m_Options.backoff_initial_ms;
    bool s_LoggedFailure = false;

    for (;;)
    {
        {
            std::lock_guard s_Lock(m_Mutex);
            if (m_Stop)
                break;
        }

        if (AsSocket(m_Socket) == INVALID_SOCKET)
        {
            if (Connect() && Handshake())
            {
                s_Backoff = m_Options.backoff_initial_ms;
                s_LoggedFailure = false;
                continue;
            }

            if (AsSocket(m_Socket) != INVALID_SOCKET)
                Disconnect("handshake failed");

            if (!s_LoggedFailure)
            {
                RelayLog::Warn("command link: not connected to {}:{}; retrying in {} ms", m_Options.host, m_Options.port, s_Backoff);
                s_LoggedFailure = true;
            }

            if (WaitStop(s_Backoff))
                break;

            s_Backoff = std::min<uint32_t>(s_Backoff * 2, m_Options.backoff_max_ms);
            continue;
        }

        // Connected and welcomed: read what BEAM sends, write what the scheduler produced.
        if (!ReadLines(50))
            continue;

        if (!FlushResults())
            continue;
    }

    Disconnect("link shutting down");
}

bool CommandLink::Connect()
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
        RelayLog::Error("command link: '{}' is not an IPv4 address; giving up", m_Options.host);
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

    const DWORD s_SendTimeout = 2000;
    setsockopt(s_Socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&s_SendTimeout), sizeof(s_SendTimeout));
    const BOOL s_NoDelay = TRUE;
    setsockopt(s_Socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&s_NoDelay), sizeof(s_NoDelay));

    m_Socket = static_cast<uintptr_t>(s_Socket);
    m_Buffer.clear();

    {
        std::lock_guard s_Lock(m_Mutex);
        m_Stats.connected = true;
        m_Stats.welcomed = false;
        m_Stats.link_generation = 0;
        ++m_Stats.connects;
    }

    RelayLog::Info("command link: connected to {}:{}", m_Options.host, m_Options.port);
    return true;
}

// Sends the hello and waits for the welcome (or a refusal) within the welcome timeout.
bool CommandLink::Handshake()
{
    CommandProtocol::Hello s_Hello;
    s_Hello.adapter_instance_id = m_AdapterInstanceId;
    s_Hello.native_at = UtcNowText();
    s_Hello.capabilities = {"relay.probe/1"};

    {
        std::lock_guard s_Lock(m_Mutex);
        s_Hello.mission_generation = m_MissionGeneration;
    }

    if (!SendLine(CommandProtocol::EncodeHello(s_Hello)))
        return false;

    const uint64_t s_Deadline = m_Clock() + m_Options.welcome_timeout_ms;

    while (m_Clock() < s_Deadline)
    {
        {
            std::lock_guard s_Lock(m_Mutex);
            if (m_Stop)
                return false;
        }

        // Read raw bytes until one line is complete.
        fd_set s_Readable;
        FD_ZERO(&s_Readable);
        FD_SET(AsSocket(m_Socket), &s_Readable);
        timeval s_Timeout = {0, 50000};

        if (select(0, &s_Readable, nullptr, nullptr, &s_Timeout) <= 0)
            continue;

        char s_Chunk[1024];
        const int s_Received = recv(AsSocket(m_Socket), s_Chunk, sizeof(s_Chunk), 0);

        if (s_Received <= 0)
        {
            RelayLog::Warn("command link: peer closed during the handshake");
            return false;
        }

        m_Buffer.append(s_Chunk, static_cast<size_t>(s_Received));

        if (m_Buffer.size() > CommandProtocol::k_MaxLineBytes + 1)
        {
            RelayLog::Warn("command link: welcome line too long");
            return false;
        }

        const size_t s_Newline = m_Buffer.find('\n');
        if (s_Newline == std::string::npos)
            continue;

        const std::string s_Line = m_Buffer.substr(0, s_Newline);
        m_Buffer.erase(0, s_Newline + 1);

        std::string s_Reason;
        if (CommandProtocol::IsRefusal(s_Line, s_Reason))
        {
            std::lock_guard s_Lock(m_Mutex);
            ++m_Stats.refusals;
            RelayLog::Warn("command link: refused by BEAM ({})", s_Reason);
            return false;
        }

        CommandProtocol::Welcome s_Welcome;
        if (!CommandProtocol::DecodeWelcome(s_Line, s_Welcome, s_Reason))
        {
            RelayLog::Warn("command link: invalid welcome ({})", s_Reason);
            return false;
        }

        {
            std::lock_guard s_Lock(m_Mutex);
            m_Stats.welcomed = true;
            m_Stats.link_generation = s_Welcome.link_generation;
        }

        RelayLog::Info("command link: welcomed by {}; link generation {}; {} allowlisted entr(y/ies)", s_Welcome.node, s_Welcome.link_generation, s_Welcome.allowlist.size());
        return true;
    }

    RelayLog::Warn("command link: no welcome within {} ms", m_Options.welcome_timeout_ms);
    return false;
}

void CommandLink::Disconnect(const char* p_Reason)
{
    if (AsSocket(m_Socket) == INVALID_SOCKET)
        return;

    closesocket(AsSocket(m_Socket));
    m_Socket = ~uintptr_t(0);
    m_Buffer.clear();

    const auto s_Dropped = m_Queue.Clear();
    size_t s_Unsent;

    {
        std::lock_guard s_Lock(m_Mutex);
        m_Stats.connected = false;
        m_Stats.welcomed = false;
        m_Stats.link_generation = 0;
        ++m_Stats.disconnects;
        m_Stats.cleared_on_disconnect += s_Dropped.size();
        s_Unsent = m_Outgoing.size();
        m_Outgoing.clear();
    }

    RelayLog::Warn("command link: disconnected ({}); {} queued command(s) invalidated, {} unsent result(s) discarded", p_Reason, s_Dropped.size(), s_Unsent);
}

// Reads whatever is available within p_WaitMs and handles every complete line. False when the
// connection ended.
bool CommandLink::ReadLines(uint32_t p_WaitMs)
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
        Disconnect("peer closed");
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
            if (m_Buffer.size() > CommandProtocol::k_MaxLineBytes)
            {
                {
                    std::lock_guard s_Lock(m_Mutex);
                    ++m_Stats.protocol_violations;
                }

                RelayLog::Warn("command link: line exceeds {} bytes; closing", CommandProtocol::k_MaxLineBytes);
                Disconnect("line too long");
                return false;
            }

            return true;
        }

        std::string s_Line = m_Buffer.substr(0, s_Newline);
        m_Buffer.erase(0, s_Newline + 1);

        if (!s_Line.empty() && s_Line.back() == '\r')
            s_Line.pop_back();

        if (s_Line.size() > CommandProtocol::k_MaxLineBytes)
        {
            {
                std::lock_guard s_Lock(m_Mutex);
                ++m_Stats.protocol_violations;
            }
            Disconnect("line too long");
            return false;
        }

        HandleLine(s_Line);

        if (AsSocket(m_Socket) == INVALID_SOCKET)
            return false;
    }
}

void CommandLink::Answer(const Result& p_Result)
{
    std::lock_guard s_Lock(m_Mutex);
    m_Outgoing.push_back(CommandProtocol::EncodeResult(p_Result));
}

// One line from BEAM after the welcome. Only commands are expected here; anything else is a
// protocol violation that closes the link (the conservative failure, as BEAM's own link does).
void CommandLink::HandleLine(std::string_view p_Line)
{
    uint64_t s_Generation;

    {
        std::lock_guard s_Lock(m_Mutex);
        ++m_Stats.lines_received;
        s_Generation = m_Stats.link_generation;
    }

    const auto s_Decoded = CommandProtocol::DecodeCommand(p_Line);

    Result s_Result;
    s_Result.command_id = s_Decoded.command_id;
    s_Result.link_generation = s_Generation;
    s_Result.adapter_instance_id = m_AdapterInstanceId;
    s_Result.native_at = UtcNowText();

    if (!s_Decoded.ok)
    {
        if (s_Decoded.command_id.empty() || s_Decoded.link_generation == 0)
        {
            // Nothing to answer with: a violation, and the link closes.
            {
                std::lock_guard s_Lock(m_Mutex);
                ++m_Stats.protocol_violations;
            }
            RelayLog::Warn("command link: protocol violation ({}); closing", s_Decoded.reason);
            Disconnect("protocol violation");
            return;
        }

        {
            std::lock_guard s_Lock(m_Mutex);
            ++m_Stats.commands_rejected;
        }

        s_Result.status = Status::Rejected;
        s_Result.reason = s_Decoded.reason;
        RelayLog::Warn("command link: rejected {} ({})", s_Decoded.command_id, s_Decoded.reason);
        Answer(s_Result);
        return;
    }

    const auto& s_Command = s_Decoded.command;

    if (s_Command.link_generation != s_Generation)
    {
        {
            std::lock_guard s_Lock(m_Mutex);
            ++m_Stats.commands_rejected;
        }

        s_Result.status = Status::Rejected;
        s_Result.reason = "link generation mismatch (command " + std::to_string(s_Command.link_generation) + ", link " + std::to_string(s_Generation) + ")";
        Answer(s_Result);
        return;
    }

    const auto s_Push = m_Queue.Push(s_Command, s_Generation, m_Clock());

    switch (s_Push.outcome)
    {
        case CommandQueue::PushOutcome::Queued:
        {
            std::lock_guard s_Lock(m_Mutex);
            ++m_Stats.commands_queued;
        }
            s_Result.status = Status::Received;
            RelayLog::Info("command link: received {} {} v{} (ttl {} ms) on generation {}", s_Command.command_id, s_Command.name, s_Command.version, s_Command.ttl_ms, s_Generation);
            Answer(s_Result);
            return;

        case CommandQueue::PushOutcome::Busy:
        {
            std::lock_guard s_Lock(m_Mutex);
            ++m_Stats.commands_busy;
        }
            s_Result.status = Status::Busy;
            s_Result.reason = "queue full (" + std::to_string(CommandQueue::k_Capacity) + ")";
            Answer(s_Result);
            return;

        case CommandQueue::PushOutcome::DuplicateQueued:
        {
            std::lock_guard s_Lock(m_Mutex);
            ++m_Stats.commands_duplicate;
        }
            s_Result.status = Status::Duplicate;
            s_Result.reason = "already queued on generation " + std::to_string(s_Push.prior_generation);
            Answer(s_Result);
            return;

        case CommandQueue::PushOutcome::DuplicateDone:
        {
            std::lock_guard s_Lock(m_Mutex);
            ++m_Stats.commands_duplicate;
        }
            s_Result.status = Status::Duplicate;
            s_Result.reason = std::string("already ") + CommandProtocol::StatusText(s_Push.prior_status) + " on generation " + std::to_string(s_Push.prior_generation) + " (bounded window of " + std::to_string(CommandQueue::k_DedupWindow) + " ids)";
            Answer(s_Result);
            return;
    }
}

bool CommandLink::FlushResults()
{
    for (;;)
    {
        std::string s_Line;

        {
            std::lock_guard s_Lock(m_Mutex);
            if (m_Outgoing.empty())
                return true;
            s_Line = std::move(m_Outgoing.front());
            m_Outgoing.pop_front();
        }

        if (!SendLine(s_Line))
            return false;

        std::lock_guard s_Lock(m_Mutex);
        ++m_Stats.results_sent;
    }
}

bool CommandLink::SendLine(const std::string& p_Line)
{
    size_t s_Offset = 0;

    while (s_Offset < p_Line.size())
    {
        const int s_Sent = send(AsSocket(m_Socket), p_Line.data() + s_Offset, static_cast<int>(p_Line.size() - s_Offset), 0);

        if (s_Sent == SOCKET_ERROR)
        {
            RelayLog::Warn("command link: send failed with error {}", WSAGetLastError());
            Disconnect("send failed");
            return false;
        }

        s_Offset += static_cast<size_t>(s_Sent);
    }

    return true;
}
