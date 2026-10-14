#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "CommandProtocol.h"
#include "CommandQueue.h"
#include "SceneState.h"

// The frame-update side of the command path (M5-B): at most one queued command per frame,
// judged on that frame's own observation. Engine-independent: the plugin hands it the frame's
// SceneState, the mission predicate and the attempt's session id as plain values; it never reads
// the engine and the only "application" in M5-B is relay.probe, which writes nothing and reports
// what the frame observed. Everything it decides becomes a result through the emit callback.
//
// Order on a frame: pop → generation check (a command from a link that is gone is dropped
// without a result; the link already reported the loss) → TTL against the native monotonic
// clock (expired: never applied) → entry dispatch → MarkDone in the queue's ring.
class CommandScheduler
{
public:
    struct FrameObservation
    {
        uint64_t frame = 0;                 // the adapter's own frame counter
        uint64_t now_mono_ms = 0;           // the native monotonic clock
        std::string native_at;              // wall-clock text for the result (audit only)
        uint32_t thread_id = 0;             // the thread invoking the scheduler, for the log and stats
        bool scene_available = false;
        SceneState scene;
        bool mission_playing = false;
        std::optional<std::string> mission_generation; // the attempt's session id when known
        uint64_t link_generation = 0;        // the link's current generation (0 = no link)
        std::string adapter_instance_id;
    };

    struct Stats
    {
        uint64_t frames = 0;
        uint64_t executed = 0;     // commands that reached dispatch (relay.probe applied)
        uint64_t expired = 0;
        uint64_t stale_generation = 0;
        uint64_t rejected = 0;
        uint32_t first_thread_id = 0;
        uint32_t last_thread_id = 0;
        bool thread_changed = false; // the scheduler was invoked from more than one thread
        uint64_t last_executed_frame = 0;
        uint64_t last_latency_ms = 0; // receipt → execution on the native monotonic clock
        uint64_t max_latency_ms = 0;
    };

    using Emit = std::function<void(const CommandProtocol::Result&)>;

    CommandScheduler(CommandQueue& p_Queue, Emit p_Emit) : m_Queue(p_Queue), m_Emit(std::move(p_Emit)) {}

    // One frame. Returns true when a command was taken from the queue this frame.
    bool Tick(const FrameObservation& p_Frame);

    Stats GetStats() const { return m_Stats; }

private:
    CommandProtocol::Result Base(const CommandQueue::Pending& p_Pending, const FrameObservation& p_Frame) const;

private:
    CommandQueue& m_Queue;
    Emit m_Emit;
    Stats m_Stats;
};
