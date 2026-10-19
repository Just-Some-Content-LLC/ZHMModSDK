#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "CandidateObservation.h"
#include "CommandProtocol.h"
#include "CommandQueue.h"
#include "IOutfitApplier.h"
#include "ITransformApplier.h"
#include "MotionController.h"
#include "OutfitObservation.h"
#include "PlayerObservation.h"
#include "PreconditionEvaluator.h"
#include "SceneState.h"
#include "TransformApply.h"
#include "TransformObservation.h"

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

    // M6-B1: the two read-only inspections of the player registry and of the local hero's authored
    // identity (relay.inspect.players / relay.inspect.entity). Same rule as the outfit inspector:
    // guarded engine reads on this frame only, nothing cached, null in tests and in the probe's
    // default mode (reported as unsupported).
    using PlayersInspector = std::function<PlayerObservation::RegistryReads()>;
    using EntityInspector = std::function<PlayerObservation::EntityReads(const PlayerObservation::EntityQuery& p_Query)>;

    // M6-D1a: the read-only candidate-hero inspection (relay.inspect.candidate). Same rule: guarded
    // engine reads on this frame only, nothing cached, null in tests and in the probe's default
    // mode (reported as unsupported).
    using CandidateInspector = std::function<CandidateObservation::CandidateReads(const CandidateObservation::CandidateQuery& p_Query)>;

    // M6-F: the read-only exact-transform inspection (relay.inspect.candidate_transform). Same rule.
    using TransformInspector = std::function<TransformObservation::Reads()>;

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

        // M6-B1 (null: the registry and entity reads are unsupported on this frame source).
        PlayersInspector players_inspector;
        EntityInspector entity_inspector;

        // M6-D1a (null: the candidate reads are unsupported on this frame source).
        CandidateInspector candidate_inspector;

        // M6-F (null: the exact transform reads are unsupported; the transform entry is rejected on
        // this frame source). The applier is null unless the plugin's effectful setting is on.
        TransformInspector transform_inspector;
        TransformApply::ITransformApplier* transform_applier = nullptr;

        // M7-B2 (null: the [motion] setting is off; the two motion entries are rejected and the M6
        // transform path is exactly M6's): the frame thread's motion controller. The scheduler
        // dispatches player.pose.share/1 and candidate.motion.session/1 to it, asks it for the M6
        // exclusion before a candidate.transform.set apply, and tells it about a setter call made on
        // this frame (at most one per frame across both paths).
        Motion::Controller* motion = nullptr;
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
        // M6-B1
        uint64_t players_inspected = 0;  // relay.inspect.players applied
        uint64_t entities_inspected = 0; // relay.inspect.entity applied
        uint64_t unbounded = 0;          // M6-B1 hardening B: a result still over the line bound after reduction (never emitted as valid; expected 0)
        // M6-D1a
        uint64_t candidates_inspected = 0; // relay.inspect.candidate applied
        // M6-F
        uint64_t transforms_inspected = 0;  // relay.inspect.candidate_transform applied
        uint64_t transform_dispatched = 0;  // candidate.transform.set v1 commands that reached the apply
        uint64_t transform_called = 0;      // the setter was invoked (once per dispatch at most)
        uint64_t transform_changed = 0;     // classified changed
        uint64_t transform_not_applied = 0; // precondition_failed or unavailable
        uint64_t transform_uncertain = 0;   // classified uncertain (incl. faults)
        // M7-B2
        uint64_t motion_dispatched = 0;     // player.pose.share/1 and candidate.motion.session/1 commands that reached the controller
        uint64_t transform_motion_held = 0; // candidate.transform.set refused because a motion session holds the candidate
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

    // M6-F: the native ledger (plain values; the process's one forward budget and the operation
    // record), readable for the probe's and the tests' reports.
    const TransformApply::Ledger& TransformLedger() const { return m_TransformLedger; }

    // M6-B1 (hardened in the M6-B1 hardening stage, correction B): reduces a result until its
    // ACTUAL encoded line — escaped strings in their encoded form, the encoder's own truncations,
    // the trailing newline — fits the protocol's bound, measuring again after every marker entry
    // and every reason note it adds, so the result the caller emits is the one that was measured.
    // Stage 1 drops the per-field reasons (every field keeps its status and value); stage 2 drops
    // plain entries from the end one at a time. Every reduction is marked by an entry
    // ("reasons_dropped", "observed_reduced") and by a note appended to the result's own reason
    // (the note always survives the encoder's reason bound). Status, command identity, link
    // identity, adapter identity, frame and mission generation are never touched. Returns true
    // when a reduction happened. Static and public so the bound is testable without a frame source.
    static bool ReduceToFit(CommandProtocol::Result& p_Result);

    // The invariant itself, on the actual encoding: the caller checks it after ReduceToFit and
    // never emits an oversized result as though it were valid.
    static bool WithinLineBound(const CommandProtocol::Result& p_Result);

private:
    CommandProtocol::Result Base(const CommandQueue::Pending& p_Pending, const FrameObservation& p_Frame) const;
    void Inspect(const CommandQueue::Pending& p_Pending, const FrameObservation& p_Frame, CommandProtocol::Result& p_Result);
    void ApplyOutfit(const CommandQueue::Pending& p_Pending, const FrameObservation& p_Frame, CommandProtocol::Result& p_Result);
    // M6-B1
    void InspectPlayers(const CommandQueue::Pending& p_Pending, const FrameObservation& p_Frame, CommandProtocol::Result& p_Result);
    void InspectEntity(const CommandQueue::Pending& p_Pending, const FrameObservation& p_Frame, CommandProtocol::Result& p_Result);
    // M6-D1a
    void InspectCandidate(const CommandQueue::Pending& p_Pending, const FrameObservation& p_Frame, CommandProtocol::Result& p_Result);
    // M6-F
    void InspectCandidateTransform(const CommandQueue::Pending& p_Pending, const FrameObservation& p_Frame, CommandProtocol::Result& p_Result);
    void ApplyTransform(const CommandQueue::Pending& p_Pending, const FrameObservation& p_Frame, CommandProtocol::Result& p_Result);
    // M7-B2: the plain frame context the apply paths take (shared by the motion entries).
    OutfitApply::Context ApplyContext(const CommandQueue::Pending& p_Pending, const FrameObservation& p_Frame) const;

private:
    CommandQueue& m_Queue;
    Emit m_Emit;
    PreconditionEvaluator::Rules m_Rules;
    Stats m_Stats;
    TransformApply::Ledger m_TransformLedger; // M6-F
};
