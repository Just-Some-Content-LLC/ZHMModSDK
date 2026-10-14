#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "OutfitObservation.h"

// The hypothetical effectful-command precondition of M5 C-1 (glacier-relay
// docs/design/M5C_EFFECTFUL_COMMAND_RESEARCH.md section 3), as a pure function over a copied frame
// observation. It performs no action and reads no engine state: the scheduler hands it plain
// values gathered on the inspection frame, and it answers whether a hypothetical effectful action
// would have been allowed and, when not, every reason why. Every reason is a closed code; the
// evaluation collects all of them rather than stopping at the first so a failing frame can be
// studied. Nothing here is a guarantee about the engine: the stable-interval constants are the
// research proposal's numbers, configurable here and documented as such, and elapsed time alone
// never satisfies the rule — a missing contract start, a changed generation or an active
// transition fails closed whatever the interval.
namespace PreconditionEvaluator
{
    struct Rules
    {
        uint64_t min_stable_ms = 1000;   // since the rise, on the native monotonic clock
        uint64_t min_stable_frames = 60; // since the rise frame
    };

    struct Input
    {
        // Generations (contract session ids as text). The command's expectation, the value the
        // frame thread had published when the network thread received the command, the edge-cached
        // value and the registry value read on this frame.
        std::optional<std::string> expected_generation;
        std::optional<std::string> generation_at_receipt;
        std::optional<std::string> generation_cached;
        OutfitObservation::Field<std::string> generation_fresh;

        // The mission predicate on this frame and the previous one, and when it rose.
        bool mission_playing = false;
        bool previous_frame_playing = false;
        uint64_t frame = 0;
        uint64_t rise_frame = 0;
        uint64_t now_mono_ms = 0;
        uint64_t rise_mono_ms = 0;

        // Scene state on this frame.
        bool scene_available = false;
        bool scene_loaded = false;
        int32_t loading_stage = -1;
        std::string scene_type;
        bool scene_resource_same_as_rise = false;

        // Transition evidence: the Relay-authored epoch now and at receipt; the last contract
        // lifecycle ids the adapter observed (never engine-authored "transition" flags).
        uint64_t epoch_now = 0;
        uint64_t epoch_at_receipt = 0;
        std::optional<std::string> contract_started_seen;
        std::optional<std::string> contract_ended_seen;

        // The hero and the candidate kit, as the inspection read them. player_available is true
        // only when the local hero entity and its interface resolved on this frame.
        bool player_available = false;
        OutfitObservation::Field<std::string> hero_outfit_id;
        OutfitObservation::Field<bool> hero_heroa_valid;
        std::optional<OutfitObservation::KitResolution> candidate;
        std::optional<std::string> expect_current;
        OutfitObservation::Field<bool> set_outfit_available;
    };

    enum class Reason
    {
        ExpectedGenerationMissing,
        ReceiptGenerationMissing,
        CachedGenerationMissing,
        FreshGenerationUnavailable,
        FreshGenerationInconsistent,
        GenerationMismatchReceipt,  // expected != at receipt
        GenerationMismatchCached,   // expected != edge-cached
        GenerationMismatchFresh,    // expected != fresh registry read
        MissionNotPlaying,
        PreviousFrameNotPlaying,
        StableIntervalTooShort,
        StableFramesTooFew,
        ContractStartMissing,       // no contract.started observed for the expected generation
        ContractStartMismatch,      // the last contract.started names another generation
        ContractEndObserved,        // a contract.ended for this generation was observed (teardown)
        SceneUnavailable,
        SceneNotLoaded,
        LoadingStageNotPlaying,
        SceneTypeNotMission,
        SceneResourceChanged,
        EpochChanged,               // the Relay transition epoch moved between receipt and this frame
        PlayerUnavailable,
        OutfitUnreadable,
        HeroAInvalid,
        CandidateMissing,           // the hypothetical action needs a candidate and none was named
        CandidateUnresolved,
        CandidateNotHeroAUsable,
        ExpectCurrentMismatch,
        SetterUnavailable,
    };

    const char* ReasonCode(Reason p_Reason); // a stable snake_case code for the result and the journal

    struct Verdict
    {
        bool ok = false;
        std::vector<Reason> reasons; // empty when ok; every failed rule otherwise, in rule order
        uint64_t stable_ms = 0;      // now - rise on the native clock (0 when not playing)
        uint64_t stable_frames = 0;  // frame - rise frame (0 when not playing)
    };

    Verdict Evaluate(const Input& p_Input, const Rules& p_Rules = Rules{});

    std::string ReasonsText(const Verdict& p_Verdict); // codes joined by ',' (bounded by the encoder)
}
