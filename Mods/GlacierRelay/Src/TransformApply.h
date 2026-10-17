#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "CommandProtocol.h"
#include "FloatWords.h"
#include "ITransformApplier.h"
#include "OutfitApply.h"
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
    Outcome Apply(const Context& p_Context, ITransformApplier& p_Applier, Ledger& p_Ledger, CommandProtocol::Result& p_Result);
}
