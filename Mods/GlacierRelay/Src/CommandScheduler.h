#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "CommandProtocol.h"
#include "CommandQueue.h"
#include "IOutfitApplier.h"
#include "OutfitObservation.h"
#include "PreconditionEvaluator.h"
#include "SceneState.h"

// The frame-update side of the command path (M5-B, extended in M5 C-1 and C-2B): at most one
// queued command per frame, judged on that frame's own observation. Engine-independent: the plugin
// hands it the frame's SceneState, the mission predicate, the attempt's session id and the
// transition instrumentation as plain values; it never reads the engine. The only engine access it
// can cause goes through the two injected interfaces, on the same frame and only when that command
// is the one taken — never on an idle frame: the inspector (relay.inspect.outfit, reads only) and
// the outfit applier (player.outfit.set v2, the one approved setter call behind the gate of
// OutfitApply; null unless the plugin's effectful setting is on, in which case the entry is
// rejected before it ever reaches a queue anyway). relay.probe reports plain frame values.
// Everything it decides becomes a result through the emit callback.
//
// Order on a frame: pop (publishing the frame's snapshot for later receipts) → generation check
// (a command from a link that is gone is dropped without a result; the link already reported the
// loss) → TTL against the native monotonic clock (expired: never applied) → entry dispatch →
// MarkDone in the queue's ring.
class CommandScheduler
{
public:
    // The plugin's frame-local inspection: given the candidate id named by the command (if any),
    // performs the guarded engine reads of this frame and returns their plain values. Null in
    // tests and in the probe's default mode, which report the engine reads as unsupported.
    using Inspector = std::function<OutfitObservation::EngineReads(const std::optional<std::string>& p_CandidateId)>;

    struct FrameObservation
    {
        uint64_t frame = 0;                 // the adapter's own frame counter
        uint64_t now_mono_ms = 0;           // the native monotonic clock
        std::string native_at;              // wall-clock text for the result (audit only)
        uint32_t thread_id = 0;             // the thread invoking the scheduler, for the log and stats
        bool scene_available = false;
        SceneState scene;
        bool mission_playing = false;
        std::optional<std::string> mission_generation; // the attempt's session id when known (edge-cached)
        uint64_t link_generation = 0;        // the link's current generation (0 = no link)
        std::string adapter_instance_id;

        // M5 C-1 transition instrumentation, maintained by the plugin (Relay-authored, never
        // engine-authored): the epoch, the previous frame's predicate, the rise, whether the scene
        // resource is still the rise's, and the last contract lifecycle ids observed.
        uint64_t relay_transition_epoch = 0;
        bool previous_frame_playing = false;
        uint64_t rise_frame = 0;
        uint64_t rise_mono_ms = 0;
        bool scene_resource_same_as_rise = false;
        std::optional<std::string> contract_started_seen;
        std::optional<std::string> contract_ended_seen;
        Inspector inspector; // null: no engine reads possible on this frame source

        // M5 C-2B: the outfit applier (null: the effectful entry is rejected as unsupported on this
        // frame source) and the link generation re-read immediately before a setter call (null:
        // the frame's copy is used, which only the tests do).
        OutfitApply::IOutfitApplier* applier = nullptr;
        std::function<uint64_t()> link_generation_now;
    };

    struct Stats
    {
        uint64_t frames = 0;
        uint64_t executed = 0;     // commands that reached dispatch (relay.probe applied)
        uint64_t inspected = 0;    // relay.inspect.outfit applied
        uint64_t reduced = 0;      // results that had to be reduced to fit the line bound
        uint64_t expired = 0;
        uint64_t stale_generation = 0;
        uint64_t rejected = 0;
        // M5 C-2B
        uint64_t outfit_dispatched = 0;  // player.outfit.set v2 commands that reached the apply
        uint64_t outfit_called = 0;      // the setter was invoked (once per dispatch at most)
        uint64_t outfit_changed = 0;     // classified changed
        uint64_t outfit_not_applied = 0; // precondition_failed or unavailable
        uint64_t outfit_uncertain = 0;   // classified uncertain (incl. faults)
        uint32_t first_thread_id = 0;
        uint32_t last_thread_id = 0;
        bool thread_changed = false; // the scheduler was invoked from more than one thread
        uint64_t last_executed_frame = 0;
        uint64_t last_latency_ms = 0; // receipt → execution on the native monotonic clock
        uint64_t max_latency_ms = 0;
    };

    using Emit = std::function<void(const CommandProtocol::Result&)>;

    // The stable-mission rule the precondition uses: the research proposal's numbers by default
    // (1,000 ms and 60 frames after a rise); the probe scales them to its fake frame rate and says
    // so. They are documented constants, not engine guarantees.
    CommandScheduler(CommandQueue& p_Queue, Emit p_Emit, PreconditionEvaluator::Rules p_Rules = PreconditionEvaluator::Rules{}) :
        m_Queue(p_Queue), m_Emit(std::move(p_Emit)), m_Rules(p_Rules) {}

    // One frame. Returns true when a command was taken from the queue this frame.
    bool Tick(const FrameObservation& p_Frame);

    Stats GetStats() const { return m_Stats; }

private:
    CommandProtocol::Result Base(const CommandQueue::Pending& p_Pending, const FrameObservation& p_Frame) const;
    void Inspect(const CommandQueue::Pending& p_Pending, const FrameObservation& p_Frame, CommandProtocol::Result& p_Result);
    void ApplyOutfit(const CommandQueue::Pending& p_Pending, const FrameObservation& p_Frame, CommandProtocol::Result& p_Result);

private:
    CommandQueue& m_Queue;
    Emit m_Emit;
    PreconditionEvaluator::Rules m_Rules;
    Stats m_Stats;
};
