#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "CommandProtocol.h"
#include "FloatWords.h"
#include "ITransformApplier.h"
#include "OutfitApply.h"
#include "PreconditionEvaluator.h"
#include "TransformObservation.h"

// The effectful apply of candidate.transform.set v1 (M6-F; glacier-relay
// docs/design/M6D2_NEXT_STEP_DESIGN.md sections 12.5–12.9). Engine-independent: it drives an
// ITransformApplier on the applying frame and never touches the engine itself. In order, on one
// frame, inside the scheduler's dispatch: (1) the common M5 gate — PreconditionEvaluator over the
// fresh reads of this frame with the outfit-specific rules switched off — and the transform gate
// (EvaluateTransform: the compiled candidate's identity, distinctness, registry absence, spatial
// interface, clean preimage, the exact expectation groups bit for bit, the compiled target, the
// ledger's budget and operation record), every failure reported with its reasons and nothing asked
// of the engine; (2) the mandatory-result capacity measured on the actual encoding before any call;
// (3) the link generation re-read; (4) the "applying" line written with the checked writer — no
// call unless the operating system accepted the whole line; (5) exactly one Invoke with the twelve
// target words; (6) the read-back (both entities re-resolved); (7) the classification over all
// twelve world words — changed / unchanged / uncertain, with the world-match, local-match and
// hero-control facts beside it — the ledger update and the bounded result. A void setter that
// returned is never, by itself, a changed state; a dirty post cache is indeterminate, never a
// match; a fault is never "definitely unapplied".
namespace TransformApply
{
    using Context = OutfitApply::Context; // the same plain frame copy the outfit apply takes
    using FloatWords::Words12;
    using FloatWords::Words7;
    using TransformObservation::Reads;
    using TransformObservation::Snapshot;

    // The native ledger (section 12.6): plain values held by the scheduler for the life of the
    // process; never an engine pointer. One forward invocation per process; one restore per
    // forward; invalidated by a fault, an uncertain forward effect, a control change, a link or
    // mission generation change at the restore, or a lost record (a new process starts at None
    // with the budget — the forward budget is consumed only by a forward call actually made).
    // M6-G1: the forward is a v1 lift or a v2 step_y — ONE budget shared by both; the record keeps
    // the forward's entry version and mode, and a restore must name the same entry version.
    struct Ledger
    {
        enum class State
        {
            None,
            ForwardInvoked,
            Restored,
            Invalidated,
        };

        uint32_t forward_budget = 1;
        State state = State::None;
        std::string forward_id;
        uint32_t forward_version = 0;                                                   // 1: a lift, 2: a step_y (0: none yet)
        CommandProtocol::TransformMode forward_mode = CommandProtocol::TransformMode::Lift;
        uint64_t link_generation = 0;
        std::string mission_generation;
        Words12 preimage_world;
        Words7 preimage_local;
        std::string preimage_parent;
        Words12 requested_world;
        Words12 postimage_world;
        Words7 postimage_local;
        std::string postimage_parent;
        bool postimage_dirty = false;
        OutfitApply::Effect forward_effect = OutfitApply::Effect::None;
        std::string invalidated_reason;
        // M7-B2: the restore's own same-frame confirmation (effect changed, all twelve world words the
        // target, the local hero's control unchanged). Recorded for the motion exclusion only (a
        // motion open is refused while a forward consumed the budget without a confirmed restoration);
        // it changes nothing in the M6 rules.
        bool restore_confirmed = false;

        void Invalidate(const std::string& p_Why)
        {
            state = State::Invalidated;
            if (invalidated_reason.empty())
                invalidated_reason = p_Why;
        }
    };

    const char* StateText(Ledger::State p_State);

    // The transform gate's closed reason codes (beside the common M5 codes of PreconditionEvaluator).
    enum class Reason
    {
        CandidateBrickAbsent,
        CandidateUnresolved,
        CandidateIndexNotInRange,
        CandidateNotHitman5,
        CandidateIdMismatch,
        CandidateOwnerMismatch,
        CandidateNotDistinct,
        CandidateSpatialUnavailable,
        CandidateSpatialNotDistinct,
        CandidateInRegistry,
        RegistryUnreadable,
        HeroControlUnavailable,
        PreimageUnavailable,
        PreimageDirty,
        ExpectWorldMismatch,
        ExpectLocalMismatch,
        ExpectParentMismatch,
        TargetEqualsCurrent,
        TargetNotFinite,
        ResultCapacity,
        BudgetExhausted,
        OperationRecordMissing,
        OperationRecordMismatch,
        OperationRecordInvalidated,
        OperationEffectNotChanged,
        RestoreTargetMismatch,
        RestoreLinkChanged,
        RestoreGenerationChanged,
        CurrentNotPostimage,
        AlreadyRestored,
        EntryModeUnsupported,  // M6-G1: not one of v1 lift | restore, v2 step_y | restore (never falls through)
        RestoreVersionMismatch, // M6-G1: a restore whose entry version is not the forward's
        MotionSessionHeld,      // M7-B2: a motion session holds the candidate (open, or retained and not released)
        RestoreParentNotOpening, // M7-B2: a motion restore whose current parent is not the session's opening parent
    };

    const char* ReasonCode(Reason p_Reason);
    bool IsUnavailability(Reason p_Reason); // "could not establish" → unavailable; otherwise precondition_failed

    struct GateInput
    {
        const CommandProtocol::TransformRequest* request = nullptr;
        const Reads* reads = nullptr;
        const Ledger* ledger = nullptr;
        uint64_t link_generation_now = 0;
        bool result_capacity_ok = true;
        bool motion_holds = false; // M7-B2: the motion controller's exclusion on this frame (false without motion)
    };

    struct GateVerdict
    {
        std::vector<Reason> reasons;   // empty when ok; every failed rule otherwise, in rule order
        std::optional<Words12> target; // the world group the one call would carry (set when the preimage was readable)
        bool ok() const { return reasons.empty(); }
    };

    // Pure: the transform rules over plain facts, so the tests exercise exactly the rule the DLL applies.
    GateVerdict EvaluateTransform(const GateInput& p_Input);
    std::string ReasonsText(const std::vector<Reason>& p_Reasons);

    // The post-call facts (section 12.8), pure over snapshots.
    enum class WorldMatch { Target, Preimage, Neither, IndeterminateDirty, Unavailable };
    enum class LocalMatch { Unchanged, Changed, Unavailable };
    enum class Control { Unchanged, WithinTolerance, Changed, Unavailable };

    const char* WorldMatchText(WorldMatch p_Match);
    const char* LocalMatchText(LocalMatch p_Match);
    const char* ControlText(Control p_Control);

    WorldMatch ClassifyWorld(const Snapshot& p_Post, const Words12& p_Preimage, const Words12& p_Target);
    LocalMatch ClassifyLocal(const Snapshot& p_Post, const Words7& p_Preimage);
    Control ClassifyControl(const Snapshot& p_Before, const Snapshot& p_After);

    // The restore facts packed into one text ("world=B,local=B,parent=B,dirty=B,snapshot=B,within_tol=B"),
    // each observed from the post read against the ledger's remembered preimage; the text is empty
    // when the post read was unavailable.
    std::string RestoreFacts(const Snapshot& p_Post, const Ledger& p_Ledger);

    // The worst case of the mandatory result on the actual encoding (section 12.3): true when a
    // result that carries every mandatory field at its bounded maximum fits the line. Exposed for
    // the test that proves the bound; Apply measures it before any call with the frame's values.
    bool MandatoryResultFits(const CommandProtocol::Result& p_Base, const Reads& p_Reads, const CommandProtocol::TransformRequest& p_Request, const Words12& p_Target);

    struct Outcome
    {
        OutfitApply::Invocation invocation = OutfitApply::Invocation::NotCalled;
        OutfitApply::Effect effect = OutfitApply::Effect::None;
        bool precondition_ok = false;
        bool reduced = false;
    };

    // Fills p_Result (status, reason, observed), updates the ledger and returns the outcome. The
    // applier is driven only when the gate holds; it is always Released before returning.
    // M7-B2: p_MotionHolds is the motion controller's exclusion on this frame (a motion session holds
    // the candidate): one more refusal of the gate, never anything else (false without motion).
    Outcome Apply(const Context& p_Context, ITransformApplier& p_Applier, Ledger& p_Ledger, CommandProtocol::Result& p_Result, bool p_MotionHolds = false);

    // ---- M7-B2: the motion session's identity rule and its restoration ----

    // The common M5 gate exactly as Apply builds it (the outfit rules switched off): the command's
    // mission_generation precondition against the receipt, cached and fresh generations, the stable
    // mission, the scene, the transition epoch since receipt, the link generation, the thread and the
    // latch. Used by the motion entries (the share, the session open and the restore).
    PreconditionEvaluator::Verdict CommonGate(const Context& p_Context, const Reads& p_Reads, uint64_t p_LinkNow, bool p_Latched);

    // The compiled candidate's identity, spatial interface, registry absence and the local hero's
    // control readability on one frame: steps 1-3 of EvaluateTransform, the same rules and reason
    // codes (the motion session's open, every pre-call check and the motion restore use it; the M6
    // gate itself is unchanged). Empty when every rule held.
    std::vector<Reason> IdentityFailures(const Reads& p_Reads);

    // The motion session's retained record, as the motion controller keeps it (plain values): what a
    // restore must name and find. Only a session closed natively with a qualified final snapshot and
    // no uncertainty is restorable.
    struct MotionRecord
    {
        std::string session_id;
        std::string open_command_id;          // restore_of must name it
        uint64_t command_link_generation = 0; // the restore must come on the same command link
        std::string mission_generation;       // the fresh generation at the open
        Words12 opening_world;                // the restore's only target
        Words7 opening_local;
        std::string opening_parent;
        Words12 final_world;                  // the qualified final snapshot of the native close
        bool restorable = false;              // closed clean with a qualified final snapshot, never restored or invalidated
        bool restored = false;
        bool invalidated = false;
    };

    // What a motion restore did to the record (the controller applies it).
    struct MotionRestoreVerdict
    {
        bool invoked = false;        // the setter was invoked (an attempted restoration, whatever its outcome)
        bool restored = false;       // invoked and every same-frame fact of design section 17.2 held natively
        bool invalidate = false;     // invoked without that proof, or a fault: never restorable again
    };

    // The restore of a motion session (candidate.motion.session/1 mode restore; design section 17.2):
    // the same order and result encoding as Apply — the common gate, IdentityFailures, the preimage
    // equal to the fresh expectation groups, the record (restorable, restore_of, the target equal to
    // the opening preimage, the current words equal to the native close's final words, the same
    // command link, the same mission generation, the opening parent), p_Holds (another session open),
    // the capacity, the link re-read, the checked "applying" line, exactly one Invoke of the opening
    // preimage, the read-back and the classification — with the M6 result's field names (mode,
    // restore_of, requested_world_words, postimage_*, world_match, hero_control, setter_called,
    // effect, ...). A non-application reports no invocation and no effect.
    Outcome ApplyMotionRestore(const Context& p_Context, ITransformApplier& p_Applier, const MotionRecord* p_Record, bool p_Holds, int64_t p_BudgetRemaining, CommandProtocol::Result& p_Result, MotionRestoreVerdict& p_Verdict);
}
