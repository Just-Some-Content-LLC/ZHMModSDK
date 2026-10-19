#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "MotionProtocol.h"

// The native end of the local motion link (M7-B2; glacier-relay docs/design/M7_NETWORK_GHOST.md
// sections 15.5, 15.6 and 17): a third outbound loopback TCP client, beside the telemetry sink and
// the command link, to BEAM's motion listener (127.0.0.1:4750). Native still never listens, and
// no remote address, TLS stack or peer credential enters the adapter: peer TLS stays between BEAM
// processes.
//
// One network thread owns the socket. It never resolves, reads or calls anything of the engine:
// it frames lines (at most 1,024 bytes, at most 1,024 buffered partial bytes), decodes them
// strictly (MotionProtocol), copies BEAM's targets and closes as plain values into a bounded
// inbound queue for the frame thread, and writes what the frame thread queued.
//
// Binding (section 15.5): it connects only while the command link is a version-2 link that sent
// its attest (CommandGeneration() != 0), sends motion.hello {adapter_instance_id,
// command_link_generation}, takes motion.welcome {motion_link_generation, nonce} within the welcome
// timeout (BEAM's binding deadline is 2 s) and answers once with motion.attest, whose MAC is
// HMAC-SHA256 with the run token over "motion|nonce|motion_link_generation|command_link_generation|
// adapter_instance_id" (Attestation::MotionAttestMac). BEAM answers a good attest with silence and
// a bad one with motion.refused and a close. The token, the nonce and the MAC are never logged.
// The binding lives only as long as this connection AND the command link generation it named: a
// command link change drops the motion link; every new connection is a new binding (a new serial)
// with a fresh welcome; nothing is reused.
//
// Bounded I/O (section 15.6; 17):
// - inbound: at most k_InboundCapacity undrained messages; more is a violation and the link closes
//   (BEAM keeps one target in flight, so a flood is never legitimate);
// - control (ghost.applied / ghost.report / ghost.closed / share.closed): at most k_ControlCapacity
//   queued lines; a full queue is refused (Queued::Full) and the caller must stop application and
//   drop the link (Drop) — required evidence is never silently discarded while writes continue;
// - samples (hero.sample): at most k_SampleCapacity queued; a full queue refuses the newest
//   (Queued::Full; the caller counts it as dropped);
// - writes: control first, then samples, each with the send deadline (SO_SNDTIMEO, 200 ms); a
//   timed-out or failed write drops the link (the frame thread then closes on link loss).
// Lost connection: the queues are discarded (counted), the binding ends (the serial is not bound
// any more) and Signals() advances, so a frame thread that is about to call the setter notices;
// the link reconnects with backoff (a new serial, a new welcome).
//
// Thread ownership: the frame thread calls Current, Drain, Signals, SendControl, SendSample and
// Drop (each takes the mutex, copies values and returns); the network thread owns everything else.
// Only started when the mod's [motion] enabled setting is true (and a run token makes the command
// link version 2); otherwise this object is never constructed.
// The frame thread's view of the motion link (MotionLink implements it; the tests and the probe's
// in-memory checks use a fake): plain values in and out, never a socket.
class IMotionLink
{
public:
    // The current binding as plain values. serial counts bound connections (0: never bound); a
    // share or session is bound to one serial and ends when it is no longer the bound one.
    struct Binding
    {
        uint64_t serial = 0;
        bool bound = false;
        uint64_t motion_generation = 0;
        uint64_t command_generation = 0;
    };

    // One BEAM message, copied as values with its receipt time (the native monotonic clock, the
    // frame thread's own clock) and the serial of the connection it arrived on.
    struct Inbound
    {
        MotionProtocol::Message message;
        uint64_t received_mono_ms = 0;
        uint64_t serial = 0;
    };

    enum class Queued
    {
        Queued,
        Full,     // the bounded queue is full: refused
        NotBound, // no bound connection, or not the serial named
    };

    virtual ~IMotionLink() = default;
    virtual Binding Current() const = 0;
    virtual std::vector<Inbound> Drain() = 0;
    // Advances (under the link's lock, with the message it signals) on every close / share.close
    // received and every binding end: a frame thread that read it before Drain and reads it again
    // immediately before a setter call cannot miss a cancellation that arrived in between.
    virtual uint64_t Signals() const = 0;
    virtual Queued SendControl(uint64_t p_Serial, std::string p_Line) = 0;
    // The control lines the bound connection still accepts (0 when not bound): the frame thread makes
    // a setter call only when the call's own evidence (its acknowledgement and a close) can be queued.
    // Only the frame thread queues control lines, so the room cannot shrink between this read and its push.
    virtual size_t ControlRoom() const = 0;
    virtual Queued SendSample(uint64_t p_Serial, std::string p_Line) = 0;
    virtual void Drop(uint64_t p_Serial, const char* p_Reason) = 0; // the frame thread cannot deliver required evidence: end this binding
};

class MotionLink final : public IMotionLink
{
public:
    struct Options
    {
        std::string host = "127.0.0.1"; // loopback only; not configurable
        uint16_t port = 4750;
        uint32_t connect_timeout_ms = 1000;
        uint32_t welcome_timeout_ms = 2000; // BEAM closes an unbound link after 2 s
        uint32_t send_timeout_ms = 200;     // the motion link's send deadline (section 15.6)
        uint32_t backoff_initial_ms = 1000;
        uint32_t backoff_max_ms = 30000;
        uint32_t stable_ms = 5000;          // a binding counts as stable, resetting the backoff, after this long (M5 N4)
        uint32_t poll_ms = 5;               // the read wait between writes
        int send_buffer_bytes = 0;          // 0: the system default (the tests set a small one to fill a socket)
        std::string token;                  // [command] token: 64 lowercase hex; never logged
    };

    static constexpr size_t k_InboundCapacity = 16;
    static constexpr size_t k_ControlCapacity = 32;
    static constexpr size_t k_SampleCapacity = 8;
    static constexpr size_t k_FlushBurst = 4; // lines written between two reads (a close is never stuck behind a long flush)

    struct Stats
    {
        bool connected = false;
        bool bound = false;
        uint64_t serial = 0;
        uint64_t motion_generation = 0;
        uint64_t command_generation = 0;
        uint64_t connect_attempts = 0;
        uint64_t connects = 0;
        uint64_t disconnects = 0;
        uint64_t welcomes = 0;
        uint64_t attests_sent = 0;
        uint64_t refusals = 0;            // motion.refused received
        uint64_t violations = 0;          // a line that is not one of BEAM's kinds, too long, out of order, or an inbound flood
        uint64_t lines_received = 0;
        uint64_t targets_received = 0;
        uint64_t closes_received = 0;
        uint64_t share_closes_received = 0;
        uint64_t control_queued = 0;
        uint64_t control_full = 0;
        uint64_t samples_queued = 0;
        uint64_t samples_full = 0;
        uint64_t lines_sent = 0;
        uint64_t send_failures = 0;       // a write that failed or missed its deadline (the link dropped)
        uint64_t discarded_on_disconnect = 0;
        uint64_t drop_requests = 0;
        uint64_t command_link_changes = 0;
        uint64_t reconnect_waits = 0;
        uint32_t current_backoff_ms = 0;  // the wait the next reconnect would take
        uint64_t stable_bindings = 0;     // bindings that stayed bound for stable_ms (the backoff reset)
        uint32_t network_thread_id = 0;
        std::string last_disconnect;
        std::string last_refusal;
    };

    using Clock = std::function<uint64_t()>;             // monotonic milliseconds (the frame thread's clock)
    using CommandGeneration = std::function<uint64_t()>; // the attested version-2 command link's generation (0: none)

    MotionLink(Options p_Options, std::string p_AdapterInstanceId, Clock p_Clock, CommandGeneration p_CommandGeneration);
    ~MotionLink();

    MotionLink(const MotionLink&) = delete;
    MotionLink& operator=(const MotionLink&) = delete;

    // Frame thread.
    Binding Current() const override;
    std::vector<Inbound> Drain() override;
    uint64_t Signals() const override { return m_Signals.load(); }
    Queued SendControl(uint64_t p_Serial, std::string p_Line) override;
    size_t ControlRoom() const override;
    Queued SendSample(uint64_t p_Serial, std::string p_Line) override;
    void Drop(uint64_t p_Serial, const char* p_Reason) override;

    Stats GetStats() const;

private:
    void Loop();
    bool Connect();
    bool Handshake(uint64_t p_CommandGeneration);
    void Disconnect(const char* p_Reason);
    bool ReadLines(uint32_t p_WaitMs);
    bool HandleLine(std::string_view p_Line);
    bool Flush();
    bool SendLine(const std::string& p_Line);
    bool WaitStop(uint32_t p_TimeoutMs);

private:
    Options m_Options;
    std::string m_AdapterInstanceId;
    Clock m_Clock;
    CommandGeneration m_CommandGeneration;

    mutable std::mutex m_Mutex;
    std::condition_variable m_Wake;
    bool m_Stop = false;
    Binding m_Binding;
    uint64_t m_DropSerial = 0;
    std::string m_DropReason;
    std::deque<Inbound> m_Inbound;
    std::deque<std::string> m_Control;
    std::deque<std::string> m_Samples;
    Stats m_Stats;
    std::atomic<uint64_t> m_Signals{0};

    std::thread m_Thread;
    uintptr_t m_Socket = ~uintptr_t(0);
    std::string m_Buffer;
    bool m_WinsockStarted = false;
};
