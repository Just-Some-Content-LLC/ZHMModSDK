#include "TransformApply.h"

#include "PreconditionEvaluator.h"
#include "RelayLog.h"

using CommandProtocol::FieldStatus;
using CommandProtocol::ObservedValue;
using CommandProtocol::Result;
using CommandProtocol::Status;
using CommandProtocol::TransformMode;
using CommandProtocol::TransformRequest;
using OutfitApply::Effect;
using OutfitApply::Invocation;
using OutfitObservation::Append;
using OutfitObservation::AppendStatus;
using OutfitObservation::Field;

namespace TransformApply
{
    const char* StateText(Ledger::State p_State)
    {
        switch (p_State)
        {
            case Ledger::State::None: return "none";
            case Ledger::State::ForwardInvoked: return "forward_invoked";
            case Ledger::State::Restored: return "restored";
            case Ledger::State::Invalidated: return "invalidated";
        }
        return "none";
    }

    const char* ReasonCode(Reason p_Reason)
    {
        switch (p_Reason)
        {
            case Reason::CandidateBrickAbsent: return "candidate_brick_absent";
            case Reason::CandidateUnresolved: return "candidate_unresolved";
            case Reason::CandidateIndexNotInRange: return "candidate_index_not_in_range";
            case Reason::CandidateNotHitman5: return "candidate_not_hitman5";
            case Reason::CandidateIdMismatch: return "candidate_id_mismatch";
            case Reason::CandidateOwnerMismatch: return "candidate_owner_mismatch";
            case Reason::CandidateNotDistinct: return "candidate_not_distinct";
            case Reason::CandidateSpatialUnavailable: return "candidate_spatial_unavailable";
            case Reason::CandidateSpatialNotDistinct: return "candidate_spatial_not_distinct";
            case Reason::CandidateInRegistry: return "candidate_in_registry";
            case Reason::RegistryUnreadable: return "registry_unreadable";
            case Reason::HeroControlUnavailable: return "hero_control_unavailable";
            case Reason::PreimageUnavailable: return "preimage_unavailable";
            case Reason::PreimageDirty: return "preimage_dirty";
            case Reason::ExpectWorldMismatch: return "expect_world_mismatch";
            case Reason::ExpectLocalMismatch: return "expect_local_mismatch";
            case Reason::ExpectParentMismatch: return "expect_parent_mismatch";
            case Reason::TargetEqualsCurrent: return "target_equals_current";
            case Reason::TargetNotFinite: return "target_not_finite";
            case Reason::ResultCapacity: return "result_capacity";
            case Reason::BudgetExhausted: return "budget_exhausted";
            case Reason::OperationRecordMissing: return "operation_record_missing";
            case Reason::OperationRecordMismatch: return "operation_record_mismatch";
            case Reason::OperationRecordInvalidated: return "operation_record_invalidated";
            case Reason::OperationEffectNotChanged: return "operation_effect_not_changed";
            case Reason::RestoreTargetMismatch: return "restore_target_mismatch";
            case Reason::RestoreLinkChanged: return "restore_link_changed";
            case Reason::RestoreGenerationChanged: return "restore_generation_changed";
            case Reason::CurrentNotPostimage: return "current_not_postimage";
            case Reason::AlreadyRestored: return "already_restored";
            case Reason::EntryModeUnsupported: return "entry_mode_unsupported";
            case Reason::RestoreVersionMismatch: return "restore_version_mismatch";
            case Reason::MotionSessionHeld: return "motion_session_held";
            case Reason::RestoreParentNotOpening: return "restore_parent_not_opening";
        }
        return "unknown";
    }

    bool IsUnavailability(Reason p_Reason)
    {
        switch (p_Reason)
        {
            case Reason::CandidateSpatialUnavailable:
            case Reason::RegistryUnreadable:
            case Reason::HeroControlUnavailable:
            case Reason::PreimageUnavailable:
            case Reason::ResultCapacity:
                return true;
            default:
                return false;
        }
    }

    std::string ReasonsText(const std::vector<Reason>& p_Reasons)
    {
        std::string s_Text;
        for (const auto s_Reason : p_Reasons)
        {
            if (!s_Text.empty())
                s_Text += ",";
            s_Text += ReasonCode(s_Reason);
        }
        return s_Text;
    }

    namespace
    {
        // M6-G1: the allowed (entry version, mode) pairs; anything else is refused by the gate.
        bool SupportedPair(const TransformRequest& p_Request)
        {
            if (p_Request.version == 1)
                return p_Request.mode == TransformMode::Lift || p_Request.mode == TransformMode::Restore;
            if (p_Request.version == 2)
                return p_Request.mode == TransformMode::StepY || p_Request.mode == TransformMode::Restore;
            return false;
        }

        // A forward is a v1 lift or a v2 step_y: the one shared budget, the operation record.
        bool IsForward(const TransformRequest& p_Request)
        {
            return p_Request.mode == TransformMode::Lift || p_Request.mode == TransformMode::StepY;
        }

        // The compiled forward target of the request's mode (only for a supported pair).
        Words12 ForwardTarget(const TransformRequest& p_Request, const Words12& p_Preimage)
        {
            return p_Request.mode == TransformMode::StepY || (p_Request.mode == TransformMode::Restore && p_Request.version == 2)
                ? TransformObservation::StepYTarget(p_Preimage)
                : TransformObservation::LiftTarget(p_Preimage);
        }
    }

    GateVerdict EvaluateTransform(const GateInput& p_Input)
    {
        GateVerdict s_Verdict;
        auto s_Fail = [&](Reason p_Reason) { s_Verdict.reasons.push_back(p_Reason); };

        if (!p_Input.request || !p_Input.reads || !p_Input.ledger)
        {
            s_Fail(Reason::PreimageUnavailable);
            return s_Verdict;
        }

        const TransformRequest& s_Request = *p_Input.request;
        const Reads& s_Reads = *p_Input.reads;
        const Ledger& s_Ledger = *p_Input.ledger;

        // 0. M6-G1: the (entry version, mode) pair must be one of v1 lift | restore, v2 step_y |
        //    restore — the decoder admits nothing else, and the gate refuses it again rather than
        //    falling through into another operation (no target is computed for it).
        const bool s_Supported = SupportedPair(s_Request);
        if (!s_Supported)
            s_Fail(Reason::EntryModeUnsupported);

        // 1. The compiled candidate's identity on this frame (M6-D1a semantics): present on a
        //    complete scan, resolved through the template factory in range, the hero interface,
        //    the authored id, the owning brick, distinct from the local hero.
        if (!(s_Reads.brick_present.observed() && *s_Reads.brick_present.value))
            s_Fail(Reason::CandidateBrickAbsent);
        if (!(s_Reads.lookup.index_check.observed() && *s_Reads.lookup.index_check.value == "in_range"))
            s_Fail(Reason::CandidateIndexNotInRange);
        if (!(s_Reads.lookup.resolved.observed() && *s_Reads.lookup.resolved.value))
            s_Fail(Reason::CandidateUnresolved);
        if (!(s_Reads.hitman5.observed() && *s_Reads.hitman5.value))
            s_Fail(Reason::CandidateNotHitman5);
        if (!(s_Reads.id_match.observed() && *s_Reads.id_match.value))
            s_Fail(Reason::CandidateIdMismatch);
        if (!(s_Reads.cand_brick_owner.observed() && *s_Reads.cand_brick_owner.value == s_Reads.brick_id))
            s_Fail(Reason::CandidateOwnerMismatch);
        if (!(s_Reads.lookup.same_entity.observed() && !*s_Reads.lookup.same_entity.value))
            s_Fail(Reason::CandidateNotDistinct);

        // 2. The spatial interface, distinct from the local hero's (only once the candidate resolved:
        //    the consequences of an unresolved candidate are not reported as further unavailability);
        //    the registry readable and the candidate in no slot (an inconclusive populated slot counts
        //    as membership not excluded).
        const bool s_Resolved = s_Verdict.reasons.empty();
        if (s_Resolved && !(s_Reads.spatial.observed() && *s_Reads.spatial.value))
            s_Fail(Reason::CandidateSpatialUnavailable);
        if (s_Resolved && s_Reads.spatial.observed() && *s_Reads.spatial.value && !(s_Reads.spatial_distinct.observed() && *s_Reads.spatial_distinct.value))
            s_Fail(Reason::CandidateSpatialNotDistinct);
        if (!s_Reads.candidate_in_registry.observed())
            s_Fail(Reason::RegistryUnreadable);
        else if (*s_Reads.candidate_in_registry.value)
            s_Fail(Reason::CandidateInRegistry);

        // 3. The local hero as the control must be readable (its words are compared after the call).
        if (!(s_Reads.hero.world.observed() && s_Reads.hero.local.observed() && s_Reads.hero.parent.observed()))
            s_Fail(Reason::HeroControlUnavailable);

        // 4. The preimage: readable, clean, and equal bit for bit to the expectation groups (its
        //    unavailability is reported only when the candidate and its spatial interface resolved).
        const Snapshot& s_Pre = s_Reads.candidate;
        const bool s_PreReadable = s_Pre.world.observed() && s_Pre.local.observed() && s_Pre.parent.observed() && s_Pre.dirty.observed();
        if (!s_PreReadable)
        {
            if (s_Resolved && s_Reads.spatial.observed() && *s_Reads.spatial.value)
                s_Fail(Reason::PreimageUnavailable);
        }
        else
        {
            if (*s_Pre.dirty.value)
                s_Fail(Reason::PreimageDirty);
            if (*s_Pre.world.value != s_Request.expect_world)
                s_Fail(Reason::ExpectWorldMismatch);
            if (*s_Pre.local.value != s_Request.expect_local)
                s_Fail(Reason::ExpectLocalMismatch);
            if (*s_Pre.parent.value != s_Request.expect_parent)
                s_Fail(Reason::ExpectParentMismatch);
        }

        // 5. The target: the compiled forward from the preimage (v1 lift: z + 0.25f; v2 step_y:
        //    y + 0.25f), or the bound restore target.
        if (s_PreReadable && s_Supported)
        {
            Words12 s_Target = s_Request.mode == TransformMode::Restore && s_Request.target_world ? *s_Request.target_world : ForwardTarget(s_Request, *s_Pre.world.value);
            s_Verdict.target = s_Target;
            if (!FloatWords::AllFinite(s_Target))
                s_Fail(Reason::TargetNotFinite);
            if (s_Target == *s_Pre.world.value)
                s_Fail(Reason::TargetEqualsCurrent);
        }

        // 6. The ledger: the one shared budget for a forward (a v1 lift or a v2 step_y); the
        //    operation record for a restore, which must name the forward's own entry version.
        if (IsForward(s_Request))
        {
            if (s_Ledger.forward_budget == 0)
                s_Fail(Reason::BudgetExhausted);
            if (s_Ledger.state == Ledger::State::Invalidated)
                s_Fail(Reason::OperationRecordInvalidated);
        }
        else
        {
            if (s_Ledger.state == Ledger::State::None)
            {
                s_Fail(Reason::OperationRecordMissing);
            }
            else
            {
                if (s_Ledger.state == Ledger::State::Invalidated)
                    s_Fail(Reason::OperationRecordInvalidated);
                if (s_Ledger.state == Ledger::State::Restored)
                    s_Fail(Reason::AlreadyRestored);
                if (s_Ledger.forward_version != s_Request.version)
                    s_Fail(Reason::RestoreVersionMismatch);
                if (!s_Request.restore_of || *s_Request.restore_of != s_Ledger.forward_id)
                    s_Fail(Reason::OperationRecordMismatch);
                if (s_Ledger.forward_effect != Effect::Changed)
                    s_Fail(Reason::OperationEffectNotChanged);
                if (!s_Request.target_world || *s_Request.target_world != s_Ledger.preimage_world)
                    s_Fail(Reason::RestoreTargetMismatch);
                if (p_Input.link_generation_now == 0 || p_Input.link_generation_now != s_Ledger.link_generation)
                    s_Fail(Reason::RestoreLinkChanged);
                if (!(s_Reads.generation_fresh.observed() && *s_Reads.generation_fresh.value == s_Ledger.mission_generation))
                    s_Fail(Reason::RestoreGenerationChanged);
                if (s_PreReadable && *s_Pre.world.value != s_Ledger.postimage_world)
                    s_Fail(Reason::CurrentNotPostimage);
            }
        }

        // M7-B2: a motion session holds the candidate (the authority's exclusivity, mirrored natively).
        if (p_Input.motion_holds)
            s_Fail(Reason::MotionSessionHeld);

        // 7. The mandatory result must be representable before any call.
        if (!p_Input.result_capacity_ok)
            s_Fail(Reason::ResultCapacity);

        return s_Verdict;
    }

    std::vector<Reason> IdentityFailures(const Reads& p_Reads)
    {
        // Steps 1-3 of EvaluateTransform, verbatim in rule and order (without its entry-mode step).
        std::vector<Reason> s_Reasons;
        auto s_Fail = [&](Reason p_Reason) { s_Reasons.push_back(p_Reason); };

        if (!(p_Reads.brick_present.observed() && *p_Reads.brick_present.value))
            s_Fail(Reason::CandidateBrickAbsent);
        if (!(p_Reads.lookup.index_check.observed() && *p_Reads.lookup.index_check.value == "in_range"))
            s_Fail(Reason::CandidateIndexNotInRange);
        if (!(p_Reads.lookup.resolved.observed() && *p_Reads.lookup.resolved.value))
            s_Fail(Reason::CandidateUnresolved);
        if (!(p_Reads.hitman5.observed() && *p_Reads.hitman5.value))
            s_Fail(Reason::CandidateNotHitman5);
        if (!(p_Reads.id_match.observed() && *p_Reads.id_match.value))
            s_Fail(Reason::CandidateIdMismatch);
        if (!(p_Reads.cand_brick_owner.observed() && *p_Reads.cand_brick_owner.value == p_Reads.brick_id))
            s_Fail(Reason::CandidateOwnerMismatch);
        if (!(p_Reads.lookup.same_entity.observed() && !*p_Reads.lookup.same_entity.value))
            s_Fail(Reason::CandidateNotDistinct);

        const bool s_Resolved = s_Reasons.empty();
        if (s_Resolved && !(p_Reads.spatial.observed() && *p_Reads.spatial.value))
            s_Fail(Reason::CandidateSpatialUnavailable);
        if (s_Resolved && p_Reads.spatial.observed() && *p_Reads.spatial.value && !(p_Reads.spatial_distinct.observed() && *p_Reads.spatial_distinct.value))
            s_Fail(Reason::CandidateSpatialNotDistinct);
        if (!p_Reads.candidate_in_registry.observed())
            s_Fail(Reason::RegistryUnreadable);
        else if (*p_Reads.candidate_in_registry.value)
            s_Fail(Reason::CandidateInRegistry);

        if (!(p_Reads.hero.world.observed() && p_Reads.hero.local.observed() && p_Reads.hero.parent.observed()))
            s_Fail(Reason::HeroControlUnavailable);

        return s_Reasons;
    }

    const char* WorldMatchText(WorldMatch p_Match)
    {
        switch (p_Match)
        {
            case WorldMatch::Target: return "target";
            case WorldMatch::Preimage: return "preimage";
            case WorldMatch::Neither: return "neither";
            case WorldMatch::IndeterminateDirty: return "indeterminate_dirty";
            case WorldMatch::Unavailable: return "unavailable";
        }
        return "unavailable";
    }

    const char* LocalMatchText(LocalMatch p_Match)
    {
        switch (p_Match)
        {
            case LocalMatch::Unchanged: return "unchanged";
            case LocalMatch::Changed: return "changed";
            case LocalMatch::Unavailable: return "unavailable";
        }
        return "unavailable";
    }

    const char* ControlText(Control p_Control)
    {
        switch (p_Control)
        {
            case Control::Unchanged: return "unchanged";
            case Control::WithinTolerance: return "within_tolerance";
            case Control::Changed: return "changed";
            case Control::Unavailable: return "unavailable";
        }
        return "unavailable";
    }

    WorldMatch ClassifyWorld(const Snapshot& p_Post, const Words12& p_Preimage, const Words12& p_Target)
    {
        if (!p_Post.world.observed() || !p_Post.dirty.observed())
            return WorldMatch::Unavailable;
        if (*p_Post.dirty.value)
            return WorldMatch::IndeterminateDirty;
        if (*p_Post.world.value == p_Target)
            return WorldMatch::Target;
        if (*p_Post.world.value == p_Preimage)
            return WorldMatch::Preimage;
        return WorldMatch::Neither;
    }

    LocalMatch ClassifyLocal(const Snapshot& p_Post, const Words7& p_Preimage)
    {
        if (!p_Post.local.observed())
            return LocalMatch::Unavailable;
        return *p_Post.local.value == p_Preimage ? LocalMatch::Unchanged : LocalMatch::Changed;
    }

    Control ClassifyControl(const Snapshot& p_Before, const Snapshot& p_After)
    {
        const bool s_Readable = p_Before.world.observed() && p_Before.local.observed() && p_Before.parent.observed()
            && p_After.world.observed() && p_After.local.observed() && p_After.parent.observed();
        if (!s_Readable)
            return Control::Unavailable;
        if (*p_Before.world.value == *p_After.world.value && *p_Before.local.value == *p_After.local.value && *p_Before.parent.value == *p_After.parent.value)
            return Control::Unchanged;
        if (*p_Before.parent.value == *p_After.parent.value && *p_Before.local.value == *p_After.local.value
            && FloatWords::WithinTolerance(*p_Before.world.value, *p_After.world.value, TransformObservation::k_DriftToleranceM))
            return Control::WithinTolerance;
        return Control::Changed;
    }

    namespace
    {
        const char* Bit(bool p_Value)
        {
            return p_Value ? "1" : "0";
        }
    }

    std::string RestoreFacts(const Snapshot& p_Post, const Ledger& p_Ledger)
    {
        if (!p_Post.world.observed() || !p_Post.local.observed() || !p_Post.parent.observed() || !p_Post.dirty.observed())
            return std::string();

        const bool s_World = *p_Post.world.value == p_Ledger.preimage_world;
        const bool s_Local = *p_Post.local.value == p_Ledger.preimage_local;
        const bool s_Parent = *p_Post.parent.value == p_Ledger.preimage_parent;
        const bool s_Dirty = !*p_Post.dirty.value;
        const bool s_Snapshot = s_World && s_Local && s_Parent && s_Dirty;
        const bool s_Within = FloatWords::WithinTolerance(*p_Post.world.value, p_Ledger.preimage_world, TransformObservation::k_DriftToleranceM);
        return fmt::format("world={},local={},parent={},dirty={},snapshot={},within_tol={}", Bit(s_World), Bit(s_Local), Bit(s_Parent), Bit(s_Dirty), Bit(s_Snapshot), Bit(s_Within));
    }

    namespace
    {
        template <class T>
        Field<T> Known(const std::optional<T>& p_Value, const char* p_Why)
        {
            return p_Value ? Field<T>::Observed(*p_Value) : Field<T>::Of(FieldStatus::Unavailable, p_Why);
        }

        void AppendText(OutfitObservation::Entries& p_Out, const char* p_Name, const std::string& p_Text)
        {
            Append(p_Out, p_Name, Field<std::string>::Observed(p_Text));
        }

        // The result that always fits: the identity, the three exact groups with their parents and
        // dirty flags, the generations, the matches, the restore binding, the invocation, the effect
        // and the verdict. Everything else is dropped and the reduction is marked.
        bool KeepWhenReduced(const std::string& p_Name)
        {
            static const char* k_Keep[] = {
                "mode", "generation_expected", "generation_at_receipt", "generation_cached", "generation_fresh", "generation_post",
                "targets", "cand_index_check", "cand_resolved", "cand_distinct", "registry_slots",
                "preimage_world_words", "preimage_local_words", "preimage_parent", "preimage_dirty",
                "requested_world_words",
                "postimage_world_words", "postimage_local_words", "postimage_parent", "postimage_dirty", "post_same_entity",
                "world_match", "local_match", "hero_control", "restore_of", "restore_facts",
                "setter_called", "apply_log", "invocation", "effect", "precondition_ok", "precondition_reasons", "budget_remaining",
            };
            for (const char* s_Name : k_Keep)
                if (p_Name == s_Name)
                    return true;
            return false;
        }

        // Every entry of the apply result in its fixed order, from plain values, so that the
        // capacity measurement and the real result share one definition.
        struct Facts
        {
            std::string mode;
            bool mission_playing = false;
            Field<std::string> scene_type;
            int64_t receipt_to_frame_ms = 0;
            Field<std::string> generation_expected, generation_at_receipt, generation_cached, generation_fresh, generation_post;
            int64_t epoch = 0, epoch_at_receipt = 0;
            const Reads* reads = nullptr;
            Snapshot preimage;
            Field<Words12> requested;
            Snapshot postimage;
            Field<bool> post_same_entity;
            Field<std::string> world_match, local_match, hero_control;
            Field<std::string> restore_of, restore_facts;
            int64_t link_generation_at_call = 0;
            bool setter_called = false;
            std::string apply_log;
            std::string invocation;
            std::string effect;
            bool precondition_ok = false;
            Field<std::string> precondition_reasons;
            int64_t budget_remaining = 0;
            Field<bool> reads_on_frame;
        };

        void AppendFacts(OutfitObservation::Entries& p_Out, const Facts& p_Facts)
        {
            AppendText(p_Out, "mode", p_Facts.mode);
            Append(p_Out, "mission_playing", Field<bool>::Observed(p_Facts.mission_playing));
            Append(p_Out, "scene_type", p_Facts.scene_type);
            Append(p_Out, "receipt_to_frame_ms", Field<int64_t>::Observed(p_Facts.receipt_to_frame_ms));
            Append(p_Out, "generation_expected", p_Facts.generation_expected);
            Append(p_Out, "generation_at_receipt", p_Facts.generation_at_receipt);
            Append(p_Out, "generation_cached", p_Facts.generation_cached);
            Append(p_Out, "generation_fresh", p_Facts.generation_fresh);
            Append(p_Out, "generation_post", p_Facts.generation_post);
            Append(p_Out, "relay_transition_epoch", Field<int64_t>::Observed(p_Facts.epoch));
            Append(p_Out, "epoch_at_receipt", Field<int64_t>::Observed(p_Facts.epoch_at_receipt));

            const Reads& s_Reads = *p_Facts.reads;
            AppendText(p_Out, "targets", s_Reads.brick_id + "," + s_Reads.entity_id);
            Append(p_Out, "cand_index_check", s_Reads.lookup.index_check);
            Append(p_Out, "cand_resolved", s_Reads.lookup.resolved);
            Append(p_Out, "cand_hitman5", s_Reads.hitman5);
            Append(p_Out, "cand_id_match", s_Reads.id_match);
            Field<bool> s_Distinct;
            if (s_Reads.lookup.same_entity.observed())
                s_Distinct = Field<bool>::Observed(!*s_Reads.lookup.same_entity.value);
            else
                s_Distinct = Field<bool>::Of(s_Reads.lookup.same_entity.status, s_Reads.lookup.same_entity.reason);
            Append(p_Out, "cand_distinct", s_Distinct);
            Append(p_Out, "cand_brick_owner", s_Reads.cand_brick_owner);
            Append(p_Out, "cand_spatial_distinct", s_Reads.spatial_distinct);
            Append(p_Out, "registry_slots", s_Reads.registry_slots);

            TransformObservation::AppendSnapshot(p_Out, "preimage_", p_Facts.preimage, false);
            TransformObservation::Append(p_Out, "requested_world_words", p_Facts.requested);
            Append(p_Out, "requested_world", p_Facts.requested.observed() ? Field<std::string>::Observed(FloatWords::TranslationText(*p_Facts.requested.value)) : Field<std::string>::Of(p_Facts.requested.status, p_Facts.requested.reason));
            TransformObservation::AppendSnapshot(p_Out, "postimage_", p_Facts.postimage, false);
            Append(p_Out, "post_same_entity", p_Facts.post_same_entity);

            Append(p_Out, "world_match", p_Facts.world_match);
            Append(p_Out, "local_match", p_Facts.local_match);
            Append(p_Out, "hero_control", p_Facts.hero_control);
            Append(p_Out, "restore_of", p_Facts.restore_of);
            Append(p_Out, "restore_facts", p_Facts.restore_facts);

            Append(p_Out, "link_generation_at_call", Field<int64_t>::Observed(p_Facts.link_generation_at_call));
            Append(p_Out, "setter_called", Field<bool>::Observed(p_Facts.setter_called));
            AppendText(p_Out, "apply_log", p_Facts.apply_log);
            AppendText(p_Out, "invocation", p_Facts.invocation);
            AppendText(p_Out, "effect", p_Facts.effect);
            Append(p_Out, "precondition_ok", Field<bool>::Observed(p_Facts.precondition_ok));
            Append(p_Out, "precondition_reasons", p_Facts.precondition_reasons);
            Append(p_Out, "budget_remaining", Field<int64_t>::Observed(p_Facts.budget_remaining));
            Append(p_Out, "reads_on_frame", p_Facts.reads_on_frame);
        }

        // The reduction shared by the real result and the capacity measurement.
        bool Reduce(Result& p_Result)
        {
            if (CommandProtocol::EncodeResult(p_Result).size() <= CommandProtocol::k_MaxLineBytes)
                return false;

            OutfitObservation::Entries s_Kept;
            for (auto& s_Entry : p_Result.observed)
                if (KeepWhenReduced(s_Entry.first))
                {
                    s_Entry.second.field_reason.clear();
                    s_Kept.push_back(s_Entry);
                }
            p_Result.observed = std::move(s_Kept);
            p_Result.observed.emplace_back("observed_reduced", [] { ObservedValue v; v.kind = ObservedValue::Kind::Bool; v.boolean = true; return v; }());
            if (p_Result.reason.size() > 120)
                p_Result.reason.resize(120);
            return true;
        }
    }

    bool MandatoryResultFits(const Result& p_Base, const Reads& p_Reads, const TransformRequest& p_Request, const Words12& p_Target)
    {
        // The post-call facts at their longest renderings, the preimage's own values, a 200-byte
        // reason: the mandatory set after the reduction must fit the line.
        Facts s_Facts;
        s_Facts.mode = CommandProtocol::TransformModeText(p_Request.mode);
        s_Facts.mission_playing = true;
        s_Facts.scene_type = Field<std::string>::Observed("mission");
        s_Facts.receipt_to_frame_ms = 99999;
        const std::string s_LongGeneration(CommandProtocol::k_MaxMissionGenerationBytes, 'g');
        const Field<std::string> s_Generation = p_Reads.generation_fresh.observed() ? p_Reads.generation_fresh : Field<std::string>::Observed(s_LongGeneration);
        s_Facts.generation_expected = s_Generation;
        s_Facts.generation_at_receipt = s_Generation;
        s_Facts.generation_cached = s_Generation;
        s_Facts.generation_fresh = s_Generation;
        s_Facts.generation_post = s_Generation;
        s_Facts.epoch = INT64_MAX;
        s_Facts.epoch_at_receipt = INT64_MAX;
        s_Facts.reads = &p_Reads;
        s_Facts.preimage = p_Reads.candidate;
        s_Facts.requested = Field<Words12>::Observed(p_Target);
        s_Facts.postimage = p_Reads.candidate; // the same lengths as any twelve-word group
        s_Facts.postimage.world = Field<Words12>::Observed(p_Target);
        s_Facts.post_same_entity = Field<bool>::Observed(true);
        s_Facts.world_match = Field<std::string>::Observed(WorldMatchText(WorldMatch::IndeterminateDirty));
        s_Facts.local_match = Field<std::string>::Observed(LocalMatchText(LocalMatch::Unavailable));
        s_Facts.hero_control = Field<std::string>::Observed(ControlText(Control::WithinTolerance));
        s_Facts.restore_of = Field<std::string>::Observed(p_Request.restore_of ? *p_Request.restore_of : std::string("00000000-0000-4000-8000-000000000000"));
        s_Facts.restore_facts = Field<std::string>::Observed("world=0,local=0,parent=0,dirty=0,snapshot=0,within_tol=0");
        s_Facts.link_generation_at_call = INT64_MAX;
        s_Facts.setter_called = true;
        s_Facts.apply_log = RelayLog::WriteStatusText(RelayLog::WriteStatus::FileUnavailable);
        s_Facts.invocation = OutfitApply::InvocationText(Invocation::NotCalled);
        s_Facts.effect = OutfitApply::EffectText(Effect::Uncertain);
        s_Facts.precondition_ok = true;
        s_Facts.precondition_reasons = Field<std::string>::Of(FieldStatus::Observed, "");
        s_Facts.budget_remaining = 0;
        s_Facts.reads_on_frame = Field<bool>::Observed(true);

        Result s_Probe = p_Base;
        s_Probe.observed.clear();
        s_Probe.reason = std::string(CommandProtocol::k_MaxReasonBytes, 'r');
        AppendFacts(s_Probe.observed, s_Facts);
        Reduce(s_Probe);
        return CommandProtocol::EncodeResult(s_Probe).size() <= CommandProtocol::k_MaxLineBytes && s_Probe.observed.size() <= CommandProtocol::k_MaxObservedEntries;
    }

    Outcome Apply(const Context& p_Context, ITransformApplier& p_Applier, Ledger& p_Ledger, Result& p_Result, bool p_MotionHolds)
    {
        Outcome s_Outcome;
        const auto& s_Pending = *p_Context.pending;
        const auto& s_Command = s_Pending.command;

        // The decoder guarantees the request for this entry; checked anyway: the apply never guesses.
        if (!s_Command.transform)
        {
            p_Result.status = Status::Rejected;
            p_Result.reason = "transform request missing";
            return s_Outcome;
        }

        const TransformRequest& s_Request = *s_Command.transform;
        const bool s_Latched = p_Applier.Latched();

        // 1. The frame-local reads (the applier holds the references for this frame only). A latched
        //    applier is not asked to read anything.
        Reads s_Reads = s_Latched ? TransformObservation::AllUnavailable(FieldStatus::Unavailable, "effectful path latched") : p_Applier.Observe();
        if (s_Latched)
            p_Ledger.Invalidate("effectful path latched");
        else if (p_Applier.Latched())
            p_Ledger.Invalidate("fault during the apply-frame reads");

        const uint64_t s_Age = p_Context.now_mono_ms >= s_Pending.received_mono_ms ? p_Context.now_mono_ms - s_Pending.received_mono_ms : 0;
        const uint64_t s_LinkNow = p_Context.link_generation_now ? p_Context.link_generation_now() : p_Context.link_generation;

        // 2. The common M5 gate (the outfit rules switched off) and the transform gate.
        PreconditionEvaluator::Input s_Input;
        s_Input.transform_gate = true;
        s_Input.expected_generation = s_Command.mission_generation_precondition;
        s_Input.generation_at_receipt = s_Pending.at_receipt.mission_generation;
        s_Input.generation_cached = p_Context.mission_generation;
        s_Input.generation_fresh = s_Reads.generation_fresh;
        s_Input.mission_playing = p_Context.mission_playing;
        s_Input.previous_frame_playing = p_Context.previous_frame_playing;
        s_Input.frame = p_Context.frame;
        s_Input.rise_frame = p_Context.rise_frame;
        s_Input.now_mono_ms = p_Context.now_mono_ms;
        s_Input.rise_mono_ms = p_Context.rise_mono_ms;
        s_Input.scene_available = p_Context.scene_available;
        s_Input.scene_loaded = p_Context.scene.scene_loaded;
        s_Input.loading_stage = p_Context.scene.loading_stage;
        s_Input.scene_type = p_Context.scene.scene_type;
        s_Input.scene_resource_same_as_rise = p_Context.scene_resource_same_as_rise;
        s_Input.epoch_now = p_Context.relay_transition_epoch;
        s_Input.epoch_at_receipt = s_Pending.at_receipt.relay_transition_epoch;
        s_Input.contract_started_seen = p_Context.contract_started_seen;
        s_Input.contract_ended_seen = p_Context.contract_ended_seen;
        s_Input.link_generation_unchanged = s_LinkNow != 0 && s_LinkNow == s_Pending.link_generation && s_LinkNow == p_Context.link_generation;
        s_Input.on_expected_thread = p_Context.expected_thread_id == 0 || p_Context.thread_id == p_Context.expected_thread_id;
        s_Input.effectful_latched = s_Latched || p_Applier.Latched();
        const auto s_Common = PreconditionEvaluator::Evaluate(s_Input, p_Context.rules);

        GateInput s_GateInput;
        s_GateInput.request = &s_Request;
        s_GateInput.reads = &s_Reads;
        s_GateInput.ledger = &p_Ledger;
        s_GateInput.link_generation_now = s_LinkNow;
        s_GateInput.motion_holds = p_MotionHolds;
        GateVerdict s_Gate = EvaluateTransform(s_GateInput);

        // M6-G1: the request's entry version must be the decoded command's own (the decoder sets
        // both; an internal disagreement is refused, never resolved in favour of either).
        if (s_Command.version != s_Request.version)
        {
            bool s_Listed = false;
            for (const auto s_Reason : s_Gate.reasons)
                s_Listed = s_Listed || s_Reason == Reason::EntryModeUnsupported;
            if (!s_Listed)
                s_Gate.reasons.push_back(Reason::EntryModeUnsupported);
        }

        // The mandatory result's capacity on the actual encoding, measured before any call with
        // this frame's values; a failure is one more refusal (nothing is called).
        if (s_Gate.target)
        {
            Result s_Base = p_Result;
            if (!MandatoryResultFits(s_Base, s_Reads, s_Request, *s_Gate.target))
                s_Gate.reasons.push_back(Reason::ResultCapacity);
        }

        const bool s_Ok = s_Common.ok && s_Gate.ok();
        s_Outcome.precondition_ok = s_Ok;

        // 3–6. The call, only when every rule held, after the link generation is re-read once more
        //      and the "applying" line was accepted by the operating system in full.
        OutfitApply::CallOutcome s_Call;
        Reads s_Post;
        bool s_PostRead = false;
        bool s_LinkChangedAtCall = false;
        bool s_ApplyLogAttempted = false;
        RelayLog::WriteOutcome s_ApplyLog;
        const Snapshot& s_Pre = s_Reads.candidate;

        if (s_Ok)
        {
            const uint64_t s_LinkAtCall = p_Context.link_generation_now ? p_Context.link_generation_now() : p_Context.link_generation;
            if (s_LinkAtCall != s_Pending.link_generation)
            {
                s_LinkChangedAtCall = true;
            }
            else
            {
                s_ApplyLogAttempted = true;
                s_ApplyLog = RelayLog::InfoChecked(
                    "command: {} applying candidate.transform.set/{} mode {} on frame {}: candidate {} in {}; generation expected {} fresh {}; epoch {}; link generation {}; preimage world {} local {} parent {} dirty 0; target world {}{}{}",
                    s_Command.command_id, s_Request.version, CommandProtocol::TransformModeText(s_Request.mode), p_Context.frame,
                    s_Reads.entity_id, s_Reads.brick_id,
                    s_Command.mission_generation_precondition ? *s_Command.mission_generation_precondition : "?",
                    s_Reads.generation_fresh.observed() ? *s_Reads.generation_fresh.value : "?",
                    p_Context.relay_transition_epoch, s_LinkAtCall,
                    FloatWords::Encode(*s_Pre.world.value), FloatWords::Encode(*s_Pre.local.value), *s_Pre.parent.value,
                    FloatWords::Encode(*s_Gate.target),
                    s_Request.restore_of ? "; restore_of " : "", s_Request.restore_of ? *s_Request.restore_of : ""
                );

                if (s_ApplyLog.status == RelayLog::WriteStatus::Written)
                {
                    // The budget is consumed by the call actually made, never by a refusal.
                    if (IsForward(s_Request))
                    {
                        p_Ledger.forward_budget = 0;
                        p_Ledger.state = Ledger::State::ForwardInvoked;
                        p_Ledger.forward_id = s_Command.command_id;
                        p_Ledger.forward_version = s_Request.version;
                        p_Ledger.forward_mode = s_Request.mode;
                        p_Ledger.link_generation = s_LinkAtCall;
                        p_Ledger.mission_generation = *s_Reads.generation_fresh.value;
                        p_Ledger.preimage_world = *s_Pre.world.value;
                        p_Ledger.preimage_local = *s_Pre.local.value;
                        p_Ledger.preimage_parent = *s_Pre.parent.value;
                        p_Ledger.requested_world = *s_Gate.target;
                        p_Ledger.forward_effect = Effect::Uncertain; // until the read-back says otherwise
                        p_Ledger.invalidated_reason.clear();
                    }
                    else
                    {
                        p_Ledger.state = Ledger::State::Restored; // one restore per forward, whatever its outcome
                    }

                    s_Call = p_Applier.Invoke(*s_Gate.target);
                    s_Post = p_Applier.ReadBack();
                    s_PostRead = true;
                }
            }
        }

        const bool s_ApplyLogUnwritten = s_ApplyLogAttempted && s_ApplyLog.status != RelayLog::WriteStatus::Written;

        p_Applier.Release();

        // 7. Classification.
        std::string s_Note;
        WorldMatch s_WorldMatch = WorldMatch::Unavailable;
        LocalMatch s_LocalMatch = LocalMatch::Unavailable;
        Control s_Control = Control::Unavailable;

        if (s_PostRead)
        {
            s_WorldMatch = ClassifyWorld(s_Post.candidate, *s_Pre.world.value, *s_Gate.target);
            s_LocalMatch = ClassifyLocal(s_Post.candidate, *s_Pre.local.value);
            s_Control = ClassifyControl(s_Reads.hero, s_Post.hero);
        }

        if (!s_Ok)
        {
            s_Outcome.invocation = Invocation::NotCalled;
            s_Outcome.effect = Effect::None;
            bool s_Unavailable = false;
            for (const auto s_Reason : s_Common.reasons)
                if (PreconditionEvaluator::IsUnavailability(s_Reason))
                    s_Unavailable = true;
            for (const auto s_Reason : s_Gate.reasons)
                if (IsUnavailability(s_Reason))
                    s_Unavailable = true;
            std::string s_Reasons = PreconditionEvaluator::ReasonsText(s_Common);
            if (!s_Gate.reasons.empty())
                s_Reasons += (s_Reasons.empty() ? "" : ",") + ReasonsText(s_Gate.reasons);
            p_Result.status = s_Unavailable ? Status::Unavailable : Status::PreconditionFailed;
            p_Result.reason = (s_Unavailable ? "not applied (unavailable): " : "not applied (precondition failed): ") + s_Reasons;
        }
        else if (s_LinkChangedAtCall)
        {
            s_Outcome.invocation = Invocation::NotCalled;
            s_Outcome.effect = Effect::None;
            p_Result.status = Status::Unavailable;
            p_Result.reason = "not applied (unavailable): link_generation_changed immediately before the call";
        }
        else if (s_ApplyLogUnwritten)
        {
            s_Outcome.invocation = Invocation::NotCalled;
            s_Outcome.effect = Effect::None;
            p_Result.status = Status::Unavailable;
            p_Result.reason = fmt::format(
                "not applied (unavailable): apply_log_unwritten ({}: {} of {} bytes accepted, error {}); the setter was not called",
                RelayLog::WriteStatusText(s_ApplyLog.status), s_ApplyLog.written, s_ApplyLog.requested, s_ApplyLog.error
            );
        }
        else if (s_Call.faulted)
        {
            s_Outcome.invocation = Invocation::Faulted;
            s_Outcome.effect = Effect::Uncertain;
            p_Result.status = Status::Failed;
            p_Result.reason = "fault inside the setter; effect uncertain (possible effect; later observation required)";
            p_Ledger.Invalidate("fault inside the setter");
        }
        else if (!s_Call.called)
        {
            s_Outcome.invocation = Invocation::NotCalled;
            s_Outcome.effect = Effect::None;
            p_Result.status = Status::Unavailable;
            p_Result.reason = "not applied (unavailable): the setter or its references were not available at the call";
            // The budget was consumed by the attempt; the record says the call was never made.
            p_Ledger.Invalidate("setter or references unavailable at the call");
        }
        else
        {
            s_Outcome.invocation = Invocation::Called;
            p_Result.status = Status::Applied;
            const bool s_GenerationHeld = s_Post.generation_fresh.observed() && s_Reads.generation_fresh.observed() && *s_Post.generation_fresh.value == *s_Reads.generation_fresh.value;
            const bool s_SameEntity = s_Post.same_entity_as_before.observed() && *s_Post.same_entity_as_before.value;
            const bool s_ParentHeld = s_Post.candidate.parent.observed() && *s_Post.candidate.parent.value == *s_Pre.parent.value;

            if (s_WorldMatch == WorldMatch::Unavailable)
            {
                s_Outcome.effect = Effect::Uncertain;
                s_Note = "setter returned; read-back unavailable (possible effect; later observation required)";
            }
            else if (!s_GenerationHeld)
            {
                s_Outcome.effect = Effect::Uncertain;
                s_Note = "setter returned; the registry generation changed or was unreadable after the call";
            }
            else if (!s_SameEntity)
            {
                s_Outcome.effect = Effect::Uncertain;
                s_Note = "setter returned; the post read did not resolve the same entity";
            }
            else if (!s_ParentHeld)
            {
                s_Outcome.effect = Effect::Uncertain;
                s_Note = "setter returned; the parent changed after the call";
            }
            else if (s_WorldMatch == WorldMatch::IndeterminateDirty)
            {
                s_Outcome.effect = Effect::Uncertain;
                s_Note = "setter returned; the post-call cache is dirty: indeterminate (the stored words establish neither the target nor the baseline)";
            }
            else if (s_WorldMatch == WorldMatch::Target)
            {
                s_Outcome.effect = Effect::Changed;
                s_Note = "setter returned; all twelve world words equal the requested words (native read-back, same frame)";
            }
            else if (s_WorldMatch == WorldMatch::Preimage)
            {
                s_Outcome.effect = Effect::Unchanged;
                s_Note = "setter returned; all twelve world words equal the preimage (no confirmed change)";
            }
            else
            {
                s_Outcome.effect = Effect::Uncertain;
                s_Note = "setter returned; the world words match neither the preimage nor the requested words";
            }

            if (s_Control != Control::Unchanged)
                s_Note += "; hero control " + std::string(ControlText(s_Control));

            p_Result.reason = s_Note;

            // The ledger: the forward effect and postimage; the invalidations of section 12.6.
            if (IsForward(s_Request))
            {
                p_Ledger.forward_effect = s_Outcome.effect;
                if (s_Post.candidate.world.observed())
                    p_Ledger.postimage_world = *s_Post.candidate.world.value;
                if (s_Post.candidate.local.observed())
                    p_Ledger.postimage_local = *s_Post.candidate.local.value;
                if (s_Post.candidate.parent.observed())
                    p_Ledger.postimage_parent = *s_Post.candidate.parent.value;
                p_Ledger.postimage_dirty = s_Post.candidate.dirty.observed() && *s_Post.candidate.dirty.value;
                if (s_Outcome.effect != Effect::Changed)
                    p_Ledger.Invalidate(std::string("forward effect ") + OutfitApply::EffectText(s_Outcome.effect));
            }
            if (s_Control != Control::Unchanged)
                p_Ledger.Invalidate(std::string("hero control ") + ControlText(s_Control));

            // M7-B2: whether the restore confirmed itself on this frame (the motion exclusion's input).
            if (s_Request.mode == TransformMode::Restore)
                p_Ledger.restore_confirmed = s_Outcome.effect == Effect::Changed && s_WorldMatch == WorldMatch::Target && s_Control == Control::Unchanged;
        }

        if (p_Applier.Latched() && p_Ledger.state != Ledger::State::Invalidated)
            p_Ledger.Invalidate("effectful path latched");

        // The result, in a fixed order.
        Facts s_Facts;
        s_Facts.mode = CommandProtocol::TransformModeText(s_Request.mode);
        s_Facts.mission_playing = p_Context.mission_playing;
        s_Facts.scene_type = p_Context.scene_available ? Field<std::string>::Observed(p_Context.scene.scene_type) : Field<std::string>::Of(FieldStatus::Unavailable, "scene state unavailable");
        s_Facts.receipt_to_frame_ms = static_cast<int64_t>(s_Age);
        s_Facts.generation_expected = Known(s_Command.mission_generation_precondition, "not supplied by the command");
        s_Facts.generation_at_receipt = Known(s_Pending.at_receipt.mission_generation, "no open attempt at receipt");
        s_Facts.generation_cached = Known(p_Context.mission_generation, "no open attempt");
        s_Facts.generation_fresh = s_Reads.generation_fresh;
        s_Facts.generation_post = s_PostRead ? s_Post.generation_fresh : Field<std::string>::Of(FieldStatus::Unavailable, "no call made");
        s_Facts.epoch = static_cast<int64_t>(p_Context.relay_transition_epoch);
        s_Facts.epoch_at_receipt = static_cast<int64_t>(s_Pending.at_receipt.relay_transition_epoch);
        s_Facts.reads = &s_Reads;
        s_Facts.preimage = s_Reads.candidate;
        s_Facts.requested = s_Gate.target ? Field<Words12>::Observed(*s_Gate.target) : Field<Words12>::Of(FieldStatus::Unavailable, "preimage unavailable");
        if (s_PostRead)
        {
            s_Facts.postimage = s_Post.candidate;
            s_Facts.post_same_entity = s_Post.same_entity_as_before;
        }
        else
        {
            TransformObservation::MarkSnapshot(s_Facts.postimage, FieldStatus::Unavailable, "no call made");
            s_Facts.post_same_entity = Field<bool>::Of(FieldStatus::Unavailable, "no call made");
        }
        s_Facts.world_match = Field<std::string>::Observed(WorldMatchText(s_WorldMatch));
        s_Facts.local_match = Field<std::string>::Observed(LocalMatchText(s_LocalMatch));
        s_Facts.hero_control = Field<std::string>::Observed(ControlText(s_Control));
        s_Facts.restore_of = s_Request.restore_of ? Field<std::string>::Observed(*s_Request.restore_of) : Field<std::string>::Of(FieldStatus::Observed, "");
        if (s_Request.mode == TransformMode::Restore && s_PostRead)
        {
            const std::string s_RestoreFacts = RestoreFacts(s_Post.candidate, p_Ledger);
            s_Facts.restore_facts = s_RestoreFacts.empty() ? Field<std::string>::Of(FieldStatus::Unavailable, "post read unavailable") : Field<std::string>::Observed(s_RestoreFacts);
        }
        else
        {
            s_Facts.restore_facts = Field<std::string>::Of(FieldStatus::Unavailable, s_Request.mode == TransformMode::Restore ? "no call made" : "not a restore");
        }
        s_Facts.link_generation_at_call = static_cast<int64_t>(s_LinkNow);
        s_Facts.setter_called = s_Call.called;
        s_Facts.apply_log = s_ApplyLogAttempted ? RelayLog::WriteStatusText(s_ApplyLog.status) : "not_attempted";
        s_Facts.invocation = OutfitApply::InvocationText(s_Outcome.invocation);
        s_Facts.effect = OutfitApply::EffectText(s_Outcome.effect);
        s_Facts.precondition_ok = s_Ok;
        if (s_Ok)
            s_Facts.precondition_reasons = Field<std::string>::Of(FieldStatus::Observed, "");
        else
        {
            std::string s_Reasons = PreconditionEvaluator::ReasonsText(s_Common);
            if (!s_Gate.reasons.empty())
                s_Reasons += (s_Reasons.empty() ? "" : ",") + ReasonsText(s_Gate.reasons);
            s_Facts.precondition_reasons = Field<std::string>::Observed(s_Reasons);
        }
        s_Facts.budget_remaining = static_cast<int64_t>(p_Ledger.forward_budget);
        s_Facts.reads_on_frame = s_Latched ? Field<bool>::Of(FieldStatus::Unavailable, "effectful path latched") : Field<bool>::Observed(s_Reads.thread_id == p_Context.thread_id);

        AppendFacts(p_Result.observed, s_Facts);

        if (Reduce(p_Result))
            s_Outcome.reduced = true;

        return s_Outcome;
    }

    PreconditionEvaluator::Verdict CommonGate(const Context& p_Context, const Reads& p_Reads, uint64_t p_LinkNow, bool p_Latched)
    {
        const auto& s_Pending = *p_Context.pending;
        PreconditionEvaluator::Input s_Input;
        s_Input.transform_gate = true;
        s_Input.expected_generation = s_Pending.command.mission_generation_precondition;
        s_Input.generation_at_receipt = s_Pending.at_receipt.mission_generation;
        s_Input.generation_cached = p_Context.mission_generation;
        s_Input.generation_fresh = p_Reads.generation_fresh;
        s_Input.mission_playing = p_Context.mission_playing;
        s_Input.previous_frame_playing = p_Context.previous_frame_playing;
        s_Input.frame = p_Context.frame;
        s_Input.rise_frame = p_Context.rise_frame;
        s_Input.now_mono_ms = p_Context.now_mono_ms;
        s_Input.rise_mono_ms = p_Context.rise_mono_ms;
        s_Input.scene_available = p_Context.scene_available;
        s_Input.scene_loaded = p_Context.scene.scene_loaded;
        s_Input.loading_stage = p_Context.scene.loading_stage;
        s_Input.scene_type = p_Context.scene.scene_type;
        s_Input.scene_resource_same_as_rise = p_Context.scene_resource_same_as_rise;
        s_Input.epoch_now = p_Context.relay_transition_epoch;
        s_Input.epoch_at_receipt = s_Pending.at_receipt.relay_transition_epoch;
        s_Input.contract_started_seen = p_Context.contract_started_seen;
        s_Input.contract_ended_seen = p_Context.contract_ended_seen;
        s_Input.link_generation_unchanged = p_LinkNow != 0 && p_LinkNow == s_Pending.link_generation && p_LinkNow == p_Context.link_generation;
        s_Input.on_expected_thread = p_Context.expected_thread_id == 0 || p_Context.thread_id == p_Context.expected_thread_id;
        s_Input.effectful_latched = p_Latched;
        return PreconditionEvaluator::Evaluate(s_Input, p_Context.rules);
    }

    Outcome ApplyMotionRestore(const Context& p_Context, ITransformApplier& p_Applier, const MotionRecord* p_Record, bool p_Holds, int64_t p_BudgetRemaining, Result& p_Result, MotionRestoreVerdict& p_Verdict)
    {
        Outcome s_Outcome;
        p_Verdict = MotionRestoreVerdict{};
        const auto& s_Pending = *p_Context.pending;
        const auto& s_Command = s_Pending.command;

        if (!s_Command.motion || s_Command.motion->mode != CommandProtocol::MotionSessionMode::Restore || !s_Command.motion->target_world || !s_Command.motion->restore_of)
        {
            p_Result.status = Status::Rejected;
            p_Result.reason = "motion restore request missing";
            return s_Outcome;
        }

        const auto& s_Motion = *s_Command.motion;
        const Words12 s_Target = *s_Motion.target_world;

        // The request as the shared result encoding sees it: a restore, never an M6 operation (the M6
        // ledger is neither read nor written here).
        TransformRequest s_Request;
        s_Request.mode = TransformMode::Restore;
        s_Request.version = 1;
        s_Request.expect_world = s_Motion.expect_world;
        s_Request.expect_local = s_Motion.expect_local;
        s_Request.expect_parent = s_Motion.expect_parent;
        s_Request.target_world = s_Target;
        s_Request.restore_of = s_Motion.restore_of;

        const bool s_Latched = p_Applier.Latched();
        Reads s_Reads = s_Latched ? TransformObservation::AllUnavailable(FieldStatus::Unavailable, "effectful path latched") : p_Applier.Observe();

        const uint64_t s_Age = p_Context.now_mono_ms >= s_Pending.received_mono_ms ? p_Context.now_mono_ms - s_Pending.received_mono_ms : 0;
        const uint64_t s_LinkNow = p_Context.link_generation_now ? p_Context.link_generation_now() : p_Context.link_generation;
        const auto s_Common = CommonGate(p_Context, s_Reads, s_LinkNow, s_Latched || p_Applier.Latched());

        // The motion restore's gate: the identity, the preimage against the fresh expectation, the
        // target, the record, the exclusion and the capacity — every failed rule, in this order.
        std::vector<Reason> s_Reasons = IdentityFailures(s_Reads);
        const Snapshot& s_Pre = s_Reads.candidate;
        const bool s_PreReadable = s_Pre.world.observed() && s_Pre.local.observed() && s_Pre.parent.observed() && s_Pre.dirty.observed();
        if (!s_PreReadable)
        {
            if (s_Reads.spatial.observed() && *s_Reads.spatial.value)
                s_Reasons.push_back(Reason::PreimageUnavailable);
        }
        else
        {
            if (*s_Pre.dirty.value)
                s_Reasons.push_back(Reason::PreimageDirty);
            if (*s_Pre.world.value != s_Motion.expect_world)
                s_Reasons.push_back(Reason::ExpectWorldMismatch);
            if (*s_Pre.local.value != s_Motion.expect_local)
                s_Reasons.push_back(Reason::ExpectLocalMismatch);
            if (*s_Pre.parent.value != s_Motion.expect_parent)
                s_Reasons.push_back(Reason::ExpectParentMismatch);
        }
        if (!FloatWords::AllFinite(s_Target))
            s_Reasons.push_back(Reason::TargetNotFinite);
        if (s_PreReadable && s_Target == *s_Pre.world.value)
            s_Reasons.push_back(Reason::TargetEqualsCurrent);

        if (!p_Record || (!p_Record->restorable && !p_Record->restored && !p_Record->invalidated))
        {
            s_Reasons.push_back(Reason::OperationRecordMissing);
        }
        else
        {
            if (p_Record->invalidated)
                s_Reasons.push_back(Reason::OperationRecordInvalidated);
            if (p_Record->restored)
                s_Reasons.push_back(Reason::AlreadyRestored);
            if (*s_Motion.restore_of != p_Record->open_command_id)
                s_Reasons.push_back(Reason::OperationRecordMismatch);
            if (s_Target != p_Record->opening_world)
                s_Reasons.push_back(Reason::RestoreTargetMismatch);
            if (s_LinkNow == 0 || s_LinkNow != p_Record->command_link_generation)
                s_Reasons.push_back(Reason::RestoreLinkChanged);
            if (!(s_Reads.generation_fresh.observed() && *s_Reads.generation_fresh.value == p_Record->mission_generation))
                s_Reasons.push_back(Reason::RestoreGenerationChanged);
            if (s_PreReadable && *s_Pre.world.value != p_Record->final_world)
                s_Reasons.push_back(Reason::CurrentNotPostimage);
            if (s_PreReadable && *s_Pre.parent.value != p_Record->opening_parent)
                s_Reasons.push_back(Reason::RestoreParentNotOpening);
        }

        if (p_Holds)
            s_Reasons.push_back(Reason::MotionSessionHeld);

        {
            Result s_Base = p_Result;
            if (!MandatoryResultFits(s_Base, s_Reads, s_Request, s_Target))
                s_Reasons.push_back(Reason::ResultCapacity);
        }

        const bool s_Ok = s_Common.ok && s_Reasons.empty();
        s_Outcome.precondition_ok = s_Ok;

        OutfitApply::CallOutcome s_Call;
        Reads s_Post;
        bool s_PostRead = false;
        bool s_LinkChangedAtCall = false;
        bool s_ApplyLogAttempted = false;
        RelayLog::WriteOutcome s_ApplyLog;

        if (s_Ok)
        {
            const uint64_t s_LinkAtCall = p_Context.link_generation_now ? p_Context.link_generation_now() : p_Context.link_generation;
            if (s_LinkAtCall != s_Pending.link_generation)
            {
                s_LinkChangedAtCall = true;
            }
            else
            {
                s_ApplyLogAttempted = true;
                s_ApplyLog = RelayLog::InfoChecked(
                    "command: {} applying candidate.motion.session/1 mode restore on frame {}: candidate {} in {}; session {} restore_of {}; generation expected {} fresh {}; epoch {}; link generation {}; preimage world {} local {} parent {} dirty 0; target world {}",
                    s_Command.command_id, p_Context.frame, s_Reads.entity_id, s_Reads.brick_id, s_Motion.session_id, *s_Motion.restore_of,
                    s_Command.mission_generation_precondition ? *s_Command.mission_generation_precondition : "?",
                    s_Reads.generation_fresh.observed() ? *s_Reads.generation_fresh.value : "?",
                    p_Context.relay_transition_epoch, s_LinkAtCall,
                    FloatWords::Encode(*s_Pre.world.value), FloatWords::Encode(*s_Pre.local.value), *s_Pre.parent.value, FloatWords::Encode(s_Target)
                );

                if (s_ApplyLog.status == RelayLog::WriteStatus::Written)
                {
                    s_Call = p_Applier.Invoke(s_Target);
                    s_Post = p_Applier.ReadBack();
                    s_PostRead = true;
                }
            }
        }

        const bool s_ApplyLogUnwritten = s_ApplyLogAttempted && s_ApplyLog.status != RelayLog::WriteStatus::Written;
        p_Applier.Release();

        WorldMatch s_WorldMatch = WorldMatch::Unavailable;
        LocalMatch s_LocalMatch = LocalMatch::Unavailable;
        Control s_Control = Control::Unavailable;
        if (s_PostRead)
        {
            s_WorldMatch = ClassifyWorld(s_Post.candidate, *s_Pre.world.value, s_Target);
            s_LocalMatch = ClassifyLocal(s_Post.candidate, *s_Pre.local.value);
            s_Control = ClassifyControl(s_Reads.hero, s_Post.hero);
        }

        std::string s_Reasons_Text = PreconditionEvaluator::ReasonsText(s_Common);
        if (!s_Reasons.empty())
            s_Reasons_Text += (s_Reasons_Text.empty() ? "" : ",") + ReasonsText(s_Reasons);

        if (!s_Ok)
        {
            bool s_Unavailable = false;
            for (const auto s_Reason : s_Common.reasons)
                if (PreconditionEvaluator::IsUnavailability(s_Reason))
                    s_Unavailable = true;
            for (const auto s_Reason : s_Reasons)
                if (IsUnavailability(s_Reason))
                    s_Unavailable = true;
            p_Result.status = s_Unavailable ? Status::Unavailable : Status::PreconditionFailed;
            p_Result.reason = (s_Unavailable ? "not applied (unavailable): " : "not applied (precondition failed): ") + s_Reasons_Text;
        }
        else if (s_LinkChangedAtCall)
        {
            p_Result.status = Status::Unavailable;
            p_Result.reason = "not applied (unavailable): link_generation_changed immediately before the call";
        }
        else if (s_ApplyLogUnwritten)
        {
            p_Result.status = Status::Unavailable;
            p_Result.reason = fmt::format(
                "not applied (unavailable): apply_log_unwritten ({}: {} of {} bytes accepted, error {}); the setter was not called",
                RelayLog::WriteStatusText(s_ApplyLog.status), s_ApplyLog.written, s_ApplyLog.requested, s_ApplyLog.error
            );
        }
        else if (s_Call.faulted)
        {
            s_Outcome.invocation = Invocation::Faulted;
            s_Outcome.effect = Effect::Uncertain;
            p_Result.status = Status::Failed;
            p_Result.reason = "fault inside the setter; effect uncertain (possible effect; later observation required)";
        }
        else if (!s_Call.called)
        {
            p_Result.status = Status::Unavailable;
            p_Result.reason = "not applied (unavailable): the setter or its references were not available at the call";
        }
        else
        {
            s_Outcome.invocation = Invocation::Called;
            p_Result.status = Status::Applied;
            const bool s_GenerationHeld = s_Post.generation_fresh.observed() && s_Reads.generation_fresh.observed() && *s_Post.generation_fresh.value == *s_Reads.generation_fresh.value;
            const bool s_SameEntity = s_Post.same_entity_as_before.observed() && *s_Post.same_entity_as_before.value;
            const bool s_ParentHeld = s_Post.candidate.parent.observed() && *s_Post.candidate.parent.value == *s_Pre.parent.value;
            std::string s_Note;

            if (s_WorldMatch == WorldMatch::Unavailable)
            {
                s_Outcome.effect = Effect::Uncertain;
                s_Note = "setter returned; read-back unavailable (possible effect; later observation required)";
            }
            else if (!s_GenerationHeld)
            {
                s_Outcome.effect = Effect::Uncertain;
                s_Note = "setter returned; the registry generation changed or was unreadable after the call";
            }
            else if (!s_SameEntity)
            {
                s_Outcome.effect = Effect::Uncertain;
                s_Note = "setter returned; the post read did not resolve the same entity";
            }
            else if (!s_ParentHeld)
            {
                s_Outcome.effect = Effect::Uncertain;
                s_Note = "setter returned; the parent changed after the call";
            }
            else if (s_WorldMatch == WorldMatch::IndeterminateDirty)
            {
                s_Outcome.effect = Effect::Uncertain;
                s_Note = "setter returned; the post-call cache is dirty: indeterminate (the stored words establish neither the target nor the baseline)";
            }
            else if (s_WorldMatch == WorldMatch::Target)
            {
                s_Outcome.effect = Effect::Changed;
                s_Note = "setter returned; all twelve world words equal the session's opening preimage (native read-back, same frame)";
            }
            else if (s_WorldMatch == WorldMatch::Preimage)
            {
                s_Outcome.effect = Effect::Unchanged;
                s_Note = "setter returned; all twelve world words equal the pre-call words (no confirmed change)";
            }
            else
            {
                s_Outcome.effect = Effect::Uncertain;
                s_Note = "setter returned; the world words match neither the pre-call words nor the requested preimage";
            }

            if (s_Control != Control::Unchanged)
                s_Note += "; hero control " + std::string(ControlText(s_Control));
            p_Result.reason = s_Note;
        }

        // The record's transition (the controller applies it): any invocation is an attempted
        // restoration; only every same-frame fact of section 17.2 makes it a restoration; anything
        // less invalidates the record for good (no second restore, no baseline release).
        p_Verdict.invoked = s_Call.called || s_Call.faulted;
        p_Verdict.restored = s_Call.called && !s_Call.faulted && s_Outcome.effect == Effect::Changed && s_WorldMatch == WorldMatch::Target
            && s_Control == Control::Unchanged && s_PostRead && s_Post.candidate.local.observed();
        p_Verdict.invalidate = p_Verdict.invoked && !p_Verdict.restored;

        Facts s_Facts;
        s_Facts.mode = "restore";
        s_Facts.mission_playing = p_Context.mission_playing;
        s_Facts.scene_type = p_Context.scene_available ? Field<std::string>::Observed(p_Context.scene.scene_type) : Field<std::string>::Of(FieldStatus::Unavailable, "scene state unavailable");
        s_Facts.receipt_to_frame_ms = static_cast<int64_t>(s_Age);
        s_Facts.generation_expected = Known(s_Command.mission_generation_precondition, "not supplied by the command");
        s_Facts.generation_at_receipt = Known(s_Pending.at_receipt.mission_generation, "no open attempt at receipt");
        s_Facts.generation_cached = Known(p_Context.mission_generation, "no open attempt");
        s_Facts.generation_fresh = s_Reads.generation_fresh;
        s_Facts.generation_post = s_PostRead ? s_Post.generation_fresh : Field<std::string>::Of(FieldStatus::Unavailable, "no call made");
        s_Facts.epoch = static_cast<int64_t>(p_Context.relay_transition_epoch);
        s_Facts.epoch_at_receipt = static_cast<int64_t>(s_Pending.at_receipt.relay_transition_epoch);
        s_Facts.reads = &s_Reads;
        s_Facts.preimage = s_Reads.candidate;
        s_Facts.requested = Field<Words12>::Observed(s_Target);
        if (s_PostRead)
        {
            s_Facts.postimage = s_Post.candidate;
            s_Facts.post_same_entity = s_Post.same_entity_as_before;
        }
        else
        {
            TransformObservation::MarkSnapshot(s_Facts.postimage, FieldStatus::Unavailable, "no call made");
            s_Facts.post_same_entity = Field<bool>::Of(FieldStatus::Unavailable, "no call made");
        }
        s_Facts.world_match = Field<std::string>::Observed(WorldMatchText(s_WorldMatch));
        s_Facts.local_match = Field<std::string>::Observed(LocalMatchText(s_LocalMatch));
        s_Facts.hero_control = Field<std::string>::Observed(ControlText(s_Control));
        s_Facts.restore_of = Field<std::string>::Observed(*s_Motion.restore_of);
        if (s_PostRead && p_Record)
        {
            Ledger s_View;
            s_View.preimage_world = p_Record->opening_world;
            s_View.preimage_local = p_Record->opening_local;
            s_View.preimage_parent = p_Record->opening_parent;
            const std::string s_RestoreFacts = RestoreFacts(s_Post.candidate, s_View);
            s_Facts.restore_facts = s_RestoreFacts.empty() ? Field<std::string>::Of(FieldStatus::Unavailable, "post read unavailable") : Field<std::string>::Observed(s_RestoreFacts);
        }
        else
        {
            s_Facts.restore_facts = Field<std::string>::Of(FieldStatus::Unavailable, "no call made");
        }
        s_Facts.link_generation_at_call = static_cast<int64_t>(s_LinkNow);
        s_Facts.setter_called = s_Call.called;
        s_Facts.apply_log = s_ApplyLogAttempted ? RelayLog::WriteStatusText(s_ApplyLog.status) : "not_attempted";
        s_Facts.invocation = OutfitApply::InvocationText(s_Outcome.invocation);
        s_Facts.effect = OutfitApply::EffectText(s_Outcome.effect);
        s_Facts.precondition_ok = s_Ok;
        s_Facts.precondition_reasons = s_Ok ? Field<std::string>::Of(FieldStatus::Observed, "") : Field<std::string>::Observed(s_Reasons_Text);
        s_Facts.budget_remaining = p_BudgetRemaining;
        s_Facts.reads_on_frame = s_Latched ? Field<bool>::Of(FieldStatus::Unavailable, "effectful path latched") : Field<bool>::Observed(s_Reads.thread_id == p_Context.thread_id);

        AppendFacts(p_Result.observed, s_Facts);
        if (Reduce(p_Result))
            s_Outcome.reduced = true;

        return s_Outcome;
    }
}
