#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "CommandProtocol.h"
#include "CommandQueue.h"

// The native end of the command link (M5-B, glacier-relay ADR 0009): a second outbound TCP client,
// separate from the telemetry sink, to BEAM's command listener on loopback. Native still never
// listens. One network thread owns the socket: it connects with a timeout and backoff, sends the
// hello, waits for the welcome and binds every later command to the welcome's link generation,
// reads command lines (at most 4,096 bytes), decodes them strictly, deduplicates and queues them,
// answers "received" (or busy / duplicate / rejected) itself, and writes the results the frame
// scheduler produced. It never reads or writes engine state. A lost connection clears the queue
// (nothing pending is applied afterwards), resets the generation to 0 and reconnects with backoff;
// a new link is a new generation, bound again by the welcome. Only started when the mod's setting
// [command] enabled is true; with it false this object is never constructed.
class CommandLink
{
public:
    struct Options
    {
        std::string host = "127.0.0.1"; // loopback only; not configurable
        uint16_t port = 4749;
        uint32_t connect_timeout_ms = 2000;
        uint32_t welcome_timeout_ms = 2000;
        uint32_t backoff_initial_ms = 1000;
        uint32_t backoff_max_ms = 30000;
    };

    struct Stats
    {
        bool connected = false;
        bool welcomed = false;
        uint64_t link_generation = 0;
        uint64_t connect_attempts = 0;
        uint64_t connects = 0;
        uint64_t disconnects = 0;
        uint64_t refusals = 0;           // BEAM answered "refused" (another link was up)
        uint64_t lines_received = 0;
        uint64_t commands_queued = 0;
        uint64_t commands_busy = 0;
        uint64_t commands_duplicate = 0;
        uint64_t commands_rejected = 0;  // decoded but not acceptable (unknown, reserved, malformed with id)
        uint64_t protocol_violations = 0; // a line that could not be answered; the link closes
        uint64_t results_sent = 0;
        uint64_t cleared_on_disconnect = 0;
        uint32_t network_thread_id = 0;
    };

    using Clock = std::function<uint64_t()>; // monotonic milliseconds

    CommandLink(Options p_Options, CommandQueue& p_Queue, std::string p_AdapterInstanceId, Clock p_Clock);
    ~CommandLink();

    CommandLink(const CommandLink&) = delete;
    CommandLink& operator=(const CommandLink&) = delete;

    // Frame thread: the result of a scheduled command, to be written by the network thread.
    void PushResult(const CommandProtocol::Result& p_Result);

    // Frame thread: the attempt's session id, reported in the hello of the next connection and
    // readable by the scheduler's caller; nullopt outside an attempt.
    void SetMissionGeneration(const std::optional<std::string>& p_Generation);

    // The generation the current link holds (0 while not welcomed). Readable from any thread.
    uint64_t LinkGeneration() const;

    Stats GetStats() const;

private:
    void Loop();
    bool Connect();
    bool Handshake();
    void Disconnect(const char* p_Reason);
    bool ReadLines(uint32_t p_WaitMs);
    void HandleLine(std::string_view p_Line);
    bool FlushResults();
    bool SendLine(const std::string& p_Line);
    bool WaitStop(uint32_t p_TimeoutMs);
    void Answer(const CommandProtocol::Result& p_Result);

private:
    Options m_Options;
    CommandQueue& m_Queue;
    std::string m_AdapterInstanceId;
    Clock m_Clock;

    mutable std::mutex m_Mutex;
    std::condition_variable m_Wake;
    std::deque<std::string> m_Outgoing; // encoded result lines
    std::optional<std::string> m_MissionGeneration;
    bool m_Stop = false;
    Stats m_Stats;

    std::thread m_Thread;
    uintptr_t m_Socket = ~uintptr_t(0);
    std::string m_Buffer; // bytes read, not yet a complete line
    bool m_WinsockStarted = false;
};
