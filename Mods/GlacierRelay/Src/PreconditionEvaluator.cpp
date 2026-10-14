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
        }

        return "unknown";
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

        // 7. The candidate kit, HeroA-usable.
        if (!p_Input.candidate)
        {
            s_Fail(Reason::CandidateMissing);
        }
        else
        {
            const auto& s_Candidate = *p_Input.candidate;
            if (!(s_Candidate.found.observed() && *s_Candidate.found.value))
                s_Fail(Reason::CandidateUnresolved);
            else if (!(s_Candidate.heroa_usable.observed() && *s_Candidate.heroa_usable.value))
                s_Fail(Reason::CandidateNotHeroAUsable);
        }

        // 8. The optional expected-current outfit id.
        if (p_Input.expect_current)
        {
            if (!p_Input.hero_outfit_id.observed() || *p_Input.hero_outfit_id.value != *p_Input.expect_current)
                s_Fail(Reason::ExpectCurrentMismatch);
        }

        // 9. The setter must exist on this build.
        if (!(p_Input.set_outfit_available.observed() && *p_Input.set_outfit_available.value))
            s_Fail(Reason::SetterUnavailable);

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
