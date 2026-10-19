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

// The native end of the command link (M5-B, glacier-relay ADR 0009; M5 C-2B versions): a second
// outbound TCP client, separate from the telemetry sink, to BEAM's command listener on loopback.
// Native still never listens. One network thread owns the socket: it connects with a timeout and
// backoff, sends the hello, waits for the welcome and binds every later command to the welcome's
// link generation, reads command lines (at most 4,096 bytes), decodes them strictly, deduplicates
// and queues them, answers "received" (or busy / duplicate / rejected) itself, and writes the
// results the frame scheduler produced. It never reads or writes engine state. A lost connection
// clears the queue (nothing pending is applied afterwards), resets the generation to 0 and
// reconnects with backoff; a new link is a new generation, bound again by the welcome.
//
// Reconnect pacing (M5 pre-C-3 hardening, finding N4): every reconnect after a connection that
// was established — refused during the handshake, closed by BEAM after the welcome (a failed
// attestation shows exactly so), or lost later — waits the current backoff first, which grows
// from backoff_initial_ms to backoff_max_ms on each consecutive retry and is reset only after a
// welcomed link stayed up for stable_ms. Before the fix a welcomed-then-closed link reconnected
// at once, about every 80 ms against a refusing BEAM. The pacing changes nothing about the
// protocol: every new link is a new generation with a fresh welcome, a fresh nonce and a fresh
// attest at the link's own version; no token or attestation is ever reused; no downgrade.
//
// Versions (M5 C-2B): with no run token configured the link speaks version 1 exactly as C-1 did.
// With a token it speaks version 2: its hello says so, the welcome must be version 2 with a
// nonce, and it answers once with an attest whose MAC covers that nonce, that generation and its
// own adapter id (Attestation::AttestMac). BEAM gives the attest no reply line: a failed
// attestation shows as BEAM closing the link, after which the link reconnects with backoff and
// attests afresh over the new welcome's nonce — never with an earlier link's attestation, never
// falling back to version 1 on the same or a later link. The token and the MAC are never logged.
//
// The effectful setting ([command] effectful, false by default) is separate: only with it on does
// the hello advertise player.outfit.set/2 and the decoder accept that entry; a second effectful
// command while one is queued is answered busy. Only started when the mod's setting [command]
// enabled is true; with it false this object is never constructed.
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
        uint32_t stable_ms = 5000;     // a welcomed link counts as stable, resetting the backoff, after this long
        bool effectful = false; // [command] effectful: advertise and decode player.outfit.set/2
        std::string token;      // [command] token: 64 lowercase hex characters → protocol version 2; empty → version 1
        bool motion = false;    // M7-B2 [motion] enabled (version 2 only): advertise and decode player.pose.share/1, and
                                // with effectful also candidate.motion.session/1; false: the hello is exactly M6's
    };

    struct Stats
    {
        bool connected = false;
        bool welcomed = false;
        uint64_t link_generation = 0;
        uint32_t protocol_version = 1;   // the version this link speaks (fixed at construction)
        bool effectful = false;
        uint64_t connect_attempts = 0;
        uint64_t connects = 0;
        uint64_t disconnects = 0;
        uint64_t refusals = 0;           // BEAM answered "refused" (another link was up)
        uint64_t attests_sent = 0;       // version 2: one per welcome
        uint64_t lines_received = 0;
        uint64_t commands_queued = 0;
        uint64_t commands_busy = 0;
        uint64_t commands_duplicate = 0;
        uint64_t commands_rejected = 0;  // decoded but not acceptable (unknown, reserved, malformed with id)
        uint64_t protocol_violations = 0; // a line that could not be answered; the link closes
        uint64_t results_sent = 0;
        uint64_t cleared_on_disconnect = 0;
        uint64_t reconnect_waits = 0;    // reconnects that waited the backoff first (N4 pacing)
        uint32_t current_backoff_ms = 0; // the wait the next reconnect would take
        uint64_t stable_links = 0;       // welcomed links that stayed up for stable_ms (backoff reset)
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

    // M7-B2: the generation of the current link when it speaks version 2 and has sent its attest over
    // this welcome (0 otherwise): the only command link a motion link may name in its hello. Whether
    // BEAM verified the attest is BEAM's to decide (it refuses the motion hello otherwise).
    uint64_t AttestedGeneration() const;

    // The version this link speaks (1 or 2), fixed at construction.
    uint32_t ProtocolVersion() const { return m_Version; }

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
    uint32_t m_Version = 1;

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
