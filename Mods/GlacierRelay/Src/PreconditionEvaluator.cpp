#include "PreconditionEvaluator.h"

namespace PreconditionEvaluator
{
    const char* ReasonCode(Reason p_Reason)
    {
        switch (p_Reason)
        {
            case Reason::ExpectedGenerationMissing: return "expected_generation_missing";
            case Reason::ReceiptGenerationMissing: return "receipt_generation_missing";
            case Reason::CachedGenerationMissing: return "cached_generation_missing";
            case Reason::FreshGenerationUnavailable: return "fresh_generation_unavailable";
            case Reason::FreshGenerationInconsistent: return "fresh_generation_inconsistent";
            case Reason::GenerationMismatchReceipt: return "generation_mismatch_receipt";
            case Reason::GenerationMismatchCached: return "generation_mismatch_cached";
            case Reason::GenerationMismatchFresh: return "generation_mismatch_fresh";
            case Reason::MissionNotPlaying: return "mission_not_playing";
            case Reason::PreviousFrameNotPlaying: return "previous_frame_not_playing";
            case Reason::StableIntervalTooShort: return "stable_interval_too_short";
            case Reason::StableFramesTooFew: return "stable_frames_too_few";
            case Reason::ContractStartMissing: return "contract_start_missing";
            case Reason::ContractStartMismatch: return "contract_start_mismatch";
            case Reason::ContractEndObserved: return "contract_end_observed";
            case Reason::SceneUnavailable: return "scene_unavailable";
            case Reason::SceneNotLoaded: return "scene_not_loaded";
            case Reason::LoadingStageNotPlaying: return "loading_stage_not_playing";
            case Reason::SceneTypeNotMission: return "scene_type_not_mission";
            case Reason::SceneResourceChanged: return "scene_resource_changed";
            case Reason::EpochChanged: return "epoch_changed";
            case Reason::PlayerUnavailable: return "player_unavailable";
            case Reason::OutfitUnreadable: return "outfit_unreadable";
            case Reason::HeroAInvalid: return "heroa_invalid";
            case Reason::CandidateMissing: return "candidate_missing";
            case Reason::CandidateUnresolved: return "candidate_unresolved";
            case Reason::CandidateNotHeroAUsable: return "candidate_not_heroa_usable";
            case Reason::ExpectCurrentMismatch: return "expect_current_mismatch";
            case Reason::SetterUnavailable: return "setter_unavailable";
            case Reason::CurrentTupleUnreadable: return "current_tuple_unreadable";
            case Reason::ExpectCurrentTupleMismatch: return "expect_current_tuple_mismatch";
            case Reason::TargetEqualsCurrent: return "target_equals_current";
            case Reason::TargetCharsetOutOfBounds: return "target_charset_out_of_bounds";
            case Reason::TargetHeroACollectionInvalid: return "target_heroa_collection_invalid";
            case Reason::TargetVariationOutOfBounds: return "target_variation_out_of_bounds";
            case Reason::LinkGenerationChanged: return "link_generation_changed";
            case Reason::ThreadMismatch: return "thread_mismatch";
            case Reason::EffectfulLatched: return "effectful_latched";
        }

        return "unknown";
    }

    bool IsUnavailability(Reason p_Reason)
    {
        switch (p_Reason)
        {
            case Reason::FreshGenerationUnavailable:
            case Reason::FreshGenerationInconsistent:
            case Reason::SceneUnavailable:
            case Reason::PlayerUnavailable:
            case Reason::OutfitUnreadable:
            case Reason::CurrentTupleUnreadable:
            case Reason::SetterUnavailable:
            case Reason::LinkGenerationChanged:
            case Reason::ThreadMismatch:
            case Reason::EffectfulLatched:
                return true;
            default:
                return false;
        }
    }

    Verdict Evaluate(const Input& p_Input, const Rules& p_Rules)
    {
        Verdict s_Verdict;
        auto s_Fail = [&](Reason p_Reason) { s_Verdict.reasons.push_back(p_Reason); };

        // 1. Generations: every one of the four must exist and agree. A missing value is a failure,
        //    never a wildcard.
        const auto& s_Expected = p_Input.expected_generation;
        if (!s_Expected)
            s_Fail(Reason::ExpectedGenerationMissing);
        if (!p_Input.generation_at_receipt)
            s_Fail(Reason::ReceiptGenerationMissing);
        if (!p_Input.generation_cached)
            s_Fail(Reason::CachedGenerationMissing);

        if (p_Input.generation_fresh.status == OutfitObservation::FieldStatus::Inconsistent)
            s_Fail(Reason::FreshGenerationInconsistent);
        else if (!p_Input.generation_fresh.observed())
            s_Fail(Reason::FreshGenerationUnavailable);

        if (s_Expected && p_Input.generation_at_receipt && *s_Expected != *p_Input.generation_at_receipt)
            s_Fail(Reason::GenerationMismatchReceipt);
        if (s_Expected && p_Input.generation_cached && *s_Expected != *p_Input.generation_cached)
            s_Fail(Reason::GenerationMismatchCached);
        if (s_Expected && p_Input.generation_fresh.observed() && *s_Expected != *p_Input.generation_fresh.value)
            s_Fail(Reason::GenerationMismatchFresh);

        // 2. The predicate, stable: playing now and on the previous frame, and long enough since
        //    the rise. The interval is a necessary condition only; the rules below still apply.
        if (!p_Input.mission_playing)
        {
            s_Fail(Reason::MissionNotPlaying);
        }
        else
        {
            if (!p_Input.previous_frame_playing)
                s_Fail(Reason::PreviousFrameNotPlaying);

            s_Verdict.stable_ms = p_Input.now_mono_ms >= p_Input.rise_mono_ms ? p_Input.now_mono_ms - p_Input.rise_mono_ms : 0;
            s_Verdict.stable_frames = p_Input.frame >= p_Input.rise_frame ? p_Input.frame - p_Input.rise_frame : 0;

            if (s_Verdict.stable_ms < p_Rules.min_stable_ms)
                s_Fail(Reason::StableIntervalTooShort);
            if (s_Verdict.stable_frames < p_Rules.min_stable_frames)
                s_Fail(Reason::StableFramesTooFew);
        }

        // 3. Contract lifecycle evidence for the expected generation: a start must have been seen
        //    for it, and no end.
        if (!p_Input.contract_started_seen)
            s_Fail(Reason::ContractStartMissing);
        else if (s_Expected && *p_Input.contract_started_seen != *s_Expected)
            s_Fail(Reason::ContractStartMismatch);

        if (p_Input.contract_ended_seen && s_Expected && *p_Input.contract_ended_seen == *s_Expected)
            s_Fail(Reason::ContractEndObserved);

        // 4. Scene state on this frame.
        if (!p_Input.scene_available)
        {
            s_Fail(Reason::SceneUnavailable);
        }
        else
        {
            if (!p_Input.scene_loaded)
                s_Fail(Reason::SceneNotLoaded);
            if (p_Input.loading_stage != 8)
                s_Fail(Reason::LoadingStageNotPlaying);
            if (p_Input.scene_type != "mission")
                s_Fail(Reason::SceneTypeNotMission);
            if (!p_Input.scene_resource_same_as_rise)
                s_Fail(Reason::SceneResourceChanged);
        }

        // 5. No transition between receipt and this frame.
        if (p_Input.epoch_now != p_Input.epoch_at_receipt)
            s_Fail(Reason::EpochChanged);

        // 6. The player and the outfit.
        if (!p_Input.player_available)
            s_Fail(Reason::PlayerUnavailable);
        else if (!p_Input.hero_outfit_id.observed())
            s_Fail(Reason::OutfitUnreadable);

        if (!(p_Input.hero_heroa_valid.observed() && *p_Input.hero_heroa_valid.value))
            s_Fail(Reason::HeroAInvalid);

        // 7. The candidate kit: HeroA-usable at charset 0 for the hypothetical rule (C-1); for an
        //    effectful request the requested charset and variation are bounds-checked instead.
        if (!p_Input.candidate)
        {
            s_Fail(Reason::CandidateMissing);
        }
        else
        {
            const auto& s_Candidate = *p_Input.candidate;
            if (!(s_Candidate.found.observed() && *s_Candidate.found.value))
            {
                s_Fail(Reason::CandidateUnresolved);
            }
            else if (!p_Input.target)
            {
                if (!(s_Candidate.heroa_usable.observed() && *s_Candidate.heroa_usable.value))
                    s_Fail(Reason::CandidateNotHeroAUsable);
            }
            else
            {
                const int32_t s_Charset = p_Input.target->charset;
                const auto& s_Counts = s_Candidate.charset_heroa_variations;
                if (s_Charset < 0 || static_cast<size_t>(s_Charset) >= s_Counts.size())
                    s_Fail(Reason::TargetCharsetOutOfBounds);
                else if (!s_Counts[static_cast<size_t>(s_Charset)].observed())
                    s_Fail(Reason::TargetHeroACollectionInvalid);
                else if (p_Input.target->variation < 0 || static_cast<int64_t>(p_Input.target->variation) >= *s_Counts[static_cast<size_t>(s_Charset)].value)
                    s_Fail(Reason::TargetVariationOutOfBounds);
            }
        }

        // 8. The optional expected-current outfit id (C-1).
        if (p_Input.expect_current)
        {
            if (!p_Input.hero_outfit_id.observed() || *p_Input.hero_outfit_id.value != *p_Input.expect_current)
                s_Fail(Reason::ExpectCurrentMismatch);
        }

        // 9. The setter must exist on this build.
        if (!(p_Input.set_outfit_available.observed() && *p_Input.set_outfit_available.value))
            s_Fail(Reason::SetterUnavailable);

        // 10. M5 C-2B, for an effectful request only: the complete current tuple readable, the
        //     mandatory expected-current tuple equal to it, the target different from it, the link
        //     and the thread unchanged, the path not latched.
        if (p_Input.target)
        {
            const bool s_TupleReadable = p_Input.hero_outfit_id.observed() && p_Input.hero_charset.observed() && p_Input.hero_variation.observed();

            if (!s_TupleReadable)
                s_Fail(Reason::CurrentTupleUnreadable);

            if (!p_Input.expect_current_tuple)
            {
                s_Fail(Reason::ExpectCurrentTupleMismatch);
            }
            else if (s_TupleReadable)
            {
                const auto& s_Expect = *p_Input.expect_current_tuple;
                if (*p_Input.hero_outfit_id.value != s_Expect.outfit_id || *p_Input.hero_charset.value != s_Expect.charset || *p_Input.hero_variation.value != s_Expect.variation)
                    s_Fail(Reason::ExpectCurrentTupleMismatch);
            }

            if (s_TupleReadable)
            {
                const auto& s_Target = *p_Input.target;
                if (*p_Input.hero_outfit_id.value == s_Target.outfit_id && *p_Input.hero_charset.value == s_Target.charset && *p_Input.hero_variation.value == s_Target.variation)
                    s_Fail(Reason::TargetEqualsCurrent);
            }

            if (!p_Input.link_generation_unchanged)
                s_Fail(Reason::LinkGenerationChanged);
            if (!p_Input.on_expected_thread)
                s_Fail(Reason::ThreadMismatch);
            if (p_Input.effectful_latched)
                s_Fail(Reason::EffectfulLatched);
        }

        s_Verdict.ok = s_Verdict.reasons.empty();
        return s_Verdict;
    }

    std::string ReasonsText(const Verdict& p_Verdict)
    {
        std::string s_Text;

        for (const auto s_Reason : p_Verdict.reasons)
        {
            if (!s_Text.empty())
                s_Text += ',';
            s_Text += ReasonCode(s_Reason);
        }

        return s_Text;
    }
}
