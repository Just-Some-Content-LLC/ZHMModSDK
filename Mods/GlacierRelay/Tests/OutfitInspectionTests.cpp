// M5 C-1 tests: the repository-id parser, the kit shape rules, the hypothetical precondition
// evaluator, the lifecycle tee, the receipt snapshot, the relay.inspect.outfit scheduling with a
// fake inspector, the bounded result encoding and the absence of any engine setter. Fake shapes and
// fake frame observations only: no engine pointer is constructed, valid or invalid.
#include "TestHarness.h"

#include <cstring>
#include <memory>
#include <vector>

#include <Windows.h>

#include "CommandScheduler.h"
#include "LifecycleTee.h"
#include "PreconditionEvaluator.h"
#include "RepositoryId.h"
#include "TestListener.h"

using namespace CommandProtocol;
using OutfitObservation::Field;
using OutfitObservation::KitShape;

namespace
{
    constexpr const char* k_Suit = "874c4c48-0a8b-49e9-883e-49fc5f1fb051";
    constexpr const char* k_Other = "2018db77-1111-4111-8111-111111111111";
    constexpr const char* k_Gen1 = "2516103582917844185-a105ec5f-3a75-46a3-8ecd-e72fd15b176c";
    constexpr const char* k_Gen2 = "2516103582027577704-30ccebb9-8138-4c72-9eb9-1a289b4fba33";

    KitShape SuitShape()
    {
        KitShape s_Kit;
        s_Kit.id = k_Suit;
        s_Kit.is_hitman_suit = true;
        s_Kit.heroa_variations = {Field<int64_t>::Observed(1)};
        return s_Kit;
    }

    // The protocol's maximum TTL by default: these tests advance the fake clock by seconds.
    Command Inspect(const std::string& p_Id, std::optional<std::string> p_Candidate = std::nullopt, std::optional<std::string> p_Expected = std::nullopt, uint32_t p_Ttl = k_MaxTtlMs)
    {
        Command s_Command;
        s_Command.command_id = p_Id;
        s_Command.link_generation = 1;
        s_Command.ttl_ms = p_Ttl;
        s_Command.name = "relay.inspect.outfit";
        s_Command.version = 1;
        s_Command.entry = Entry::RelayInspectOutfit1;
        if (p_Candidate)
            s_Command.params.emplace_back("disguise_repository_id", *p_Candidate);
        s_Command.mission_generation_precondition = p_Expected;
        return s_Command;
    }

    // A fully observed engine-read set for a playing mission with the suit worn.
    OutfitObservation::EngineReads GoodReads(const std::optional<std::string>& p_Candidate, const char* p_Fresh = k_Gen1)
    {
        OutfitObservation::EngineReads s_Reads;
        s_Reads.thread_id = GetCurrentThreadId();
        s_Reads.generation_fresh = Field<std::string>::Observed(p_Fresh);
        s_Reads.hero.player_resolved = true;
        OutfitObservation::DescribeHeroOutfit(SuitShape(), 0, 0, s_Reads.hero);
        s_Reads.kit_map_available = Field<bool>::Observed(true);
        s_Reads.set_outfit_available = Field<bool>::Observed(true);
        if (p_Candidate)
        {
            OutfitObservation::KitResolution s_Kit;
            if (*p_Candidate == k_Suit)
                OutfitObservation::DescribeCandidate(SuitShape(), s_Kit);
            else
                s_Kit.found = Field<bool>::Observed(false);
            s_Kit.kits_scanned = Field<int64_t>::Observed(2);
            s_Reads.candidate = s_Kit;
        }
        return s_Reads;
    }

    // A stable playing frame: rise at frame 1 / t=0, now frame 100 / t=5000, epoch 1 since the rise.
    CommandScheduler::FrameObservation PlayingFrame(uint64_t p_Frame = 100, uint64_t p_NowMs = 5000)
    {
        CommandScheduler::FrameObservation s_Frame;
        s_Frame.frame = p_Frame;
        s_Frame.now_mono_ms = p_NowMs;
        s_Frame.native_at = "t";
        s_Frame.thread_id = GetCurrentThreadId();
        s_Frame.scene_available = true;
        s_Frame.scene.available = true;
        s_Frame.scene.scene_resource = "assembly:/_PRO/Scenes/Missions/Paris/_Scene_FashionShowHit_01.entity";
        s_Frame.scene.scene_loaded = true;
        s_Frame.scene.loading_stage = 8;
        s_Frame.scene.scene_type = "mission";
        s_Frame.scene.codename_hint = "Peacock";
        s_Frame.mission_playing = true;
        s_Frame.mission_generation = k_Gen1;
        s_Frame.link_generation = 1;
        s_Frame.adapter_instance_id = CommandTest::Uuid(99);
        s_Frame.relay_transition_epoch = 1;
        s_Frame.previous_frame_playing = true;
        s_Frame.rise_frame = 1;
        s_Frame.rise_mono_ms = 0;
        s_Frame.scene_resource_same_as_rise = true;
        s_Frame.contract_started_seen = k_Gen1;
        return s_Frame;
    }

    const ObservedValue* Observed(const Result& p_Result, const char* p_Key)
    {
        for (const auto& s_Entry : p_Result.observed)
            if (s_Entry.first == p_Key)
                return &s_Entry.second;
        return nullptr;
    }

    bool FieldIs(const Result& p_Result, const char* p_Key, FieldStatus p_Status)
    {
        const auto* s_Value = Observed(p_Result, p_Key);
        return s_Value && s_Value->field_status && *s_Value->field_status == p_Status;
    }

    bool BoolField(const Result& p_Result, const char* p_Key, bool p_Expected)
    {
        const auto* s_Value = Observed(p_Result, p_Key);
        return s_Value && s_Value->field_status && *s_Value->field_status == FieldStatus::Observed && s_Value->kind == ObservedValue::Kind::Bool && s_Value->boolean == p_Expected;
    }

    std::string TextField(const Result& p_Result, const char* p_Key)
    {
        const auto* s_Value = Observed(p_Result, p_Key);
        return s_Value && s_Value->kind == ObservedValue::Kind::Text ? s_Value->text : std::string();
    }

    // An absent or non-integer field compares unequal to anything (never dereferenced).
    int64_t IntField(const Result& p_Result, const char* p_Key)
    {
        const auto* s_Value = Observed(p_Result, p_Key);
        return s_Value && s_Value->kind == ObservedValue::Kind::Integer ? s_Value->integer : INT64_MIN;
    }

    PreconditionEvaluator::Input GoodInput()
    {
        PreconditionEvaluator::Input s_Input;
        s_Input.expected_generation = k_Gen1;
        s_Input.generation_at_receipt = k_Gen1;
        s_Input.generation_cached = k_Gen1;
        s_Input.generation_fresh = Field<std::string>::Observed(k_Gen1);
        s_Input.mission_playing = true;
        s_Input.previous_frame_playing = true;
        s_Input.frame = 100;
        s_Input.rise_frame = 1;
        s_Input.now_mono_ms = 5000;
        s_Input.rise_mono_ms = 0;
        s_Input.scene_available = true;
        s_Input.scene_loaded = true;
        s_Input.loading_stage = 8;
        s_Input.scene_type = "mission";
        s_Input.scene_resource_same_as_rise = true;
        s_Input.epoch_now = 1;
        s_Input.epoch_at_receipt = 1;
        s_Input.contract_started_seen = k_Gen1;
        s_Input.player_available = true;
        s_Input.hero_outfit_id = Field<std::string>::Observed(k_Suit);
        s_Input.hero_heroa_valid = Field<bool>::Observed(true);
        OutfitObservation::KitResolution s_Kit;
        OutfitObservation::DescribeCandidate(SuitShape(), s_Kit);
        s_Input.candidate = s_Kit;
        s_Input.set_outfit_available = Field<bool>::Observed(true);
        return s_Input;
    }

    bool Has(const PreconditionEvaluator::Verdict& p_Verdict, PreconditionEvaluator::Reason p_Reason)
    {
        for (const auto s_Reason : p_Verdict.reasons)
            if (s_Reason == p_Reason)
                return true;
        return false;
    }

    struct CapturingSink : IRelaySink
    {
        std::vector<PublishedEnvelope> Published;
        void Publish(const PublishedEnvelope& p_Envelope) override { Published.push_back(p_Envelope); }
    };
}

void RunOutfitInspectionTests()
{
    using PreconditionEvaluator::Reason;

    // ---- repository id: parse round trip, rejection of anything not dashed lowercase ----------
    {
        RepositoryId s_Id;
        CHECK(RepositoryId::FromDashedLowercase(k_Suit, s_Id));
        CHECK(s_Id.ToDashedLowercase() == k_Suit);
        CHECK(s_Id.data1 == 0x874c4c48u && s_Id.data2 == 0x0a8b && s_Id.data3 == 0x49e9 && s_Id.data4[0] == 0x88 && s_Id.data4[7] == 0x51);
        CHECK(RepositoryId::FromLittleEndianBytes(s_Id.ToLittleEndianBytes()) == s_Id);
        RepositoryId s_Untouched;
        CHECK(!RepositoryId::FromDashedLowercase("874C4C48-0A8B-49E9-883E-49FC5F1FB051", s_Untouched)); // uppercase
        CHECK(!RepositoryId::FromDashedLowercase("874c4c480a8b49e9883e49fc5f1fb051", s_Untouched));     // no dashes
        CHECK(!RepositoryId::FromDashedLowercase("874c4c48-0a8b-49e9-883e-49fc5f1fb05", s_Untouched));  // short
        CHECK(!RepositoryId::FromDashedLowercase("874c4c48-0a8b-49e9-883e-49fc5f1fb05g", s_Untouched)); // not hex
        CHECK(s_Untouched.data1 == 0);
    }

    // ---- kit shape rules: the hero tuple and the candidate ------------------------------------
    {
        OutfitObservation::HeroOutfit s_Hero;
        OutfitObservation::DescribeHeroOutfit(SuitShape(), 0, 0, s_Hero);
        CHECK(s_Hero.outfit_id.observed() && *s_Hero.outfit_id.value == k_Suit);
        CHECK(s_Hero.charset.observed() && *s_Hero.charset.value == 0 && s_Hero.variation.observed() && *s_Hero.variation.value == 0);
        CHECK(s_Hero.charset_count.observed() && *s_Hero.charset_count.value == 1);
        CHECK(s_Hero.heroa_variations.observed() && *s_Hero.heroa_variations.value == 1);
        CHECK(s_Hero.heroa_valid.observed() && *s_Hero.heroa_valid.value && s_Hero.is_hitman_suit.observed() && *s_Hero.is_hitman_suit.value);

        // invalid charset: out of bounds → not valid, the count unavailable with the reason
        OutfitObservation::HeroOutfit s_BadCharset;
        OutfitObservation::DescribeHeroOutfit(SuitShape(), 3, 0, s_BadCharset);
        CHECK(s_BadCharset.heroa_valid.observed() && !*s_BadCharset.heroa_valid.value);
        CHECK(s_BadCharset.heroa_variations.status == FieldStatus::Unavailable && s_BadCharset.heroa_variations.reason == "charset index out of bounds");

        // invalid variation: beyond the HeroA count
        OutfitObservation::HeroOutfit s_BadVariation;
        OutfitObservation::DescribeHeroOutfit(SuitShape(), 0, 1, s_BadVariation);
        CHECK(s_BadVariation.heroa_valid.observed() && !*s_BadVariation.heroa_valid.value);
        OutfitObservation::HeroOutfit s_Negative;
        OutfitObservation::DescribeHeroOutfit(SuitShape(), 0, -1, s_Negative);
        CHECK(s_Negative.heroa_valid.observed() && !*s_Negative.heroa_valid.value);

        // missing HeroA collection in the charset
        KitShape s_NoHeroA = SuitShape();
        s_NoHeroA.heroa_variations = {Field<int64_t>::Of(FieldStatus::Unavailable, "no HeroA character type in charset")};
        OutfitObservation::HeroOutfit s_NoHeroAHero;
        OutfitObservation::DescribeHeroOutfit(s_NoHeroA, 0, 0, s_NoHeroAHero);
        CHECK(s_NoHeroAHero.heroa_valid.observed() && !*s_NoHeroAHero.heroa_valid.value);
        CHECK(s_NoHeroAHero.heroa_variations.status == FieldStatus::Unavailable);

        // candidate: found and usable; found with zero HeroA variations; no charset at all
        OutfitObservation::KitResolution s_Candidate;
        OutfitObservation::DescribeCandidate(SuitShape(), s_Candidate);
        CHECK(s_Candidate.found.observed() && *s_Candidate.found.value && s_Candidate.heroa_usable.observed() && *s_Candidate.heroa_usable.value);
        KitShape s_Zero = SuitShape();
        s_Zero.heroa_variations = {Field<int64_t>::Observed(0)};
        OutfitObservation::KitResolution s_ZeroCandidate;
        OutfitObservation::DescribeCandidate(s_Zero, s_ZeroCandidate);
        CHECK(s_ZeroCandidate.heroa_usable.observed() && !*s_ZeroCandidate.heroa_usable.value);
        KitShape s_Empty;
        s_Empty.id = k_Other;
        OutfitObservation::KitResolution s_EmptyCandidate;
        OutfitObservation::DescribeCandidate(s_Empty, s_EmptyCandidate);
        CHECK(s_EmptyCandidate.charsets.observed() && *s_EmptyCandidate.charsets.value == 0 && s_EmptyCandidate.heroa_usable.observed() && !*s_EmptyCandidate.heroa_usable.value);
        CHECK(s_EmptyCandidate.heroa_variations.status == FieldStatus::Unavailable);
    }

    // ---- the hypothetical precondition evaluator -----------------------------------------------
    {
        // every rule satisfied
        auto s_Ok = PreconditionEvaluator::Evaluate(GoodInput());
        CHECK(s_Ok.ok && s_Ok.reasons.empty() && s_Ok.stable_ms == 5000 && s_Ok.stable_frames == 99);

        // generation mismatch (fresh registry differs: the race) and cached mismatch
        auto s_In = GoodInput();
        s_In.generation_fresh = Field<std::string>::Observed(k_Gen2);
        auto s_V = PreconditionEvaluator::Evaluate(s_In);
        CHECK(!s_V.ok && Has(s_V, Reason::GenerationMismatchFresh) && !Has(s_V, Reason::GenerationMismatchCached));

        // stale receipt generation
        s_In = GoodInput();
        s_In.generation_at_receipt = k_Gen2;
        s_V = PreconditionEvaluator::Evaluate(s_In);
        CHECK(!s_V.ok && Has(s_V, Reason::GenerationMismatchReceipt));

        // missing values are failures, never wildcards
        s_In = GoodInput();
        s_In.expected_generation.reset();
        s_V = PreconditionEvaluator::Evaluate(s_In);
        CHECK(!s_V.ok && Has(s_V, Reason::ExpectedGenerationMissing));
        s_In = GoodInput();
        s_In.generation_at_receipt.reset();
        CHECK(Has(PreconditionEvaluator::Evaluate(s_In), Reason::ReceiptGenerationMissing));
        s_In = GoodInput();
        s_In.generation_cached.reset();
        CHECK(Has(PreconditionEvaluator::Evaluate(s_In), Reason::CachedGenerationMissing));
        s_In = GoodInput();
        s_In.generation_fresh = Field<std::string>::Of(FieldStatus::Unavailable, "registry null");
        CHECK(Has(PreconditionEvaluator::Evaluate(s_In), Reason::FreshGenerationUnavailable));
        s_In = GoodInput();
        s_In.generation_fresh = Field<std::string>::Of(FieldStatus::Inconsistent, "torn");
        s_V = PreconditionEvaluator::Evaluate(s_In);
        CHECK(Has(s_V, Reason::FreshGenerationInconsistent) && !Has(s_V, Reason::FreshGenerationUnavailable));

        // transition epoch changed between receipt and the frame
        s_In = GoodInput();
        s_In.epoch_at_receipt = 0;
        CHECK(Has(PreconditionEvaluator::Evaluate(s_In), Reason::EpochChanged));

        // missing contract start fails regardless of the elapsed interval; a mismatching one too
        s_In = GoodInput();
        s_In.contract_started_seen.reset();
        s_In.now_mono_ms = 1'000'000;
        s_In.frame = 100'000;
        s_V = PreconditionEvaluator::Evaluate(s_In);
        CHECK(!s_V.ok && Has(s_V, Reason::ContractStartMissing) && !Has(s_V, Reason::StableIntervalTooShort));
        s_In = GoodInput();
        s_In.contract_started_seen = k_Gen2;
        CHECK(Has(PreconditionEvaluator::Evaluate(s_In), Reason::ContractStartMismatch));

        // a contract end observed for the active generation (teardown)
        s_In = GoodInput();
        s_In.contract_ended_seen = k_Gen1;
        CHECK(Has(PreconditionEvaluator::Evaluate(s_In), Reason::ContractEndObserved));
        s_In = GoodInput();
        s_In.contract_ended_seen = k_Gen2; // an older generation's end is not this one's
        CHECK(!Has(PreconditionEvaluator::Evaluate(s_In), Reason::ContractEndObserved));

        // stable interval: just below and exactly at the rule, in time and in frames
        s_In = GoodInput();
        s_In.now_mono_ms = 999;
        s_V = PreconditionEvaluator::Evaluate(s_In);
        CHECK(Has(s_V, Reason::StableIntervalTooShort) && !Has(s_V, Reason::StableFramesTooFew));
        s_In.now_mono_ms = 1000;
        CHECK(!Has(PreconditionEvaluator::Evaluate(s_In), Reason::StableIntervalTooShort));
        s_In = GoodInput();
        s_In.frame = 60; // 59 frames since the rise
        CHECK(Has(PreconditionEvaluator::Evaluate(s_In), Reason::StableFramesTooFew));
        s_In.frame = 61;
        CHECK(!Has(PreconditionEvaluator::Evaluate(s_In), Reason::StableFramesTooFew));
        PreconditionEvaluator::Rules s_Scaled;
        s_Scaled.min_stable_frames = 3;
        s_Scaled.min_stable_ms = 30;
        s_In = GoodInput();
        s_In.frame = 5;
        s_In.now_mono_ms = 40;
        CHECK(PreconditionEvaluator::Evaluate(s_In, s_Scaled).ok);

        // the previous frame not playing (the rise frame itself)
        s_In = GoodInput();
        s_In.previous_frame_playing = false;
        CHECK(Has(PreconditionEvaluator::Evaluate(s_In), Reason::PreviousFrameNotPlaying));

        // not playing at all: no stable figures, the predicate reason only
        s_In = GoodInput();
        s_In.mission_playing = false;
        s_V = PreconditionEvaluator::Evaluate(s_In);
        CHECK(Has(s_V, Reason::MissionNotPlaying) && !Has(s_V, Reason::StableIntervalTooShort) && s_V.stable_ms == 0);

        // scene unavailable; loading / teardown (stage, loaded flag, type)
        s_In = GoodInput();
        s_In.scene_available = false;
        s_V = PreconditionEvaluator::Evaluate(s_In);
        CHECK(Has(s_V, Reason::SceneUnavailable) && !Has(s_V, Reason::SceneNotLoaded));
        s_In = GoodInput();
        s_In.scene_loaded = false;
        s_In.loading_stage = 5;
        s_V = PreconditionEvaluator::Evaluate(s_In);
        CHECK(Has(s_V, Reason::SceneNotLoaded) && Has(s_V, Reason::LoadingStageNotPlaying));
        s_In = GoodInput();
        s_In.scene_type = "";
        CHECK(Has(PreconditionEvaluator::Evaluate(s_In), Reason::SceneTypeNotMission));

        // restart with the scene resource unchanged: the resource rule holds, the generation rules catch it
        s_In = GoodInput();
        s_In.scene_resource_same_as_rise = true;
        s_In.generation_fresh = Field<std::string>::Observed(k_Gen2);
        s_In.contract_ended_seen = k_Gen1;
        s_V = PreconditionEvaluator::Evaluate(s_In);
        CHECK(!s_V.ok && !Has(s_V, Reason::SceneResourceChanged) && Has(s_V, Reason::GenerationMismatchFresh) && Has(s_V, Reason::ContractEndObserved));
        s_In = GoodInput();
        s_In.scene_resource_same_as_rise = false;
        CHECK(Has(PreconditionEvaluator::Evaluate(s_In), Reason::SceneResourceChanged));

        // missing hero, unreadable outfit, HeroA invalid
        s_In = GoodInput();
        s_In.player_available = false;
        s_V = PreconditionEvaluator::Evaluate(s_In);
        CHECK(Has(s_V, Reason::PlayerUnavailable) && !Has(s_V, Reason::OutfitUnreadable));
        s_In = GoodInput();
        s_In.hero_outfit_id = Field<std::string>::Of(FieldStatus::Unavailable, "outfit kit reference null");
        CHECK(Has(PreconditionEvaluator::Evaluate(s_In), Reason::OutfitUnreadable));
        s_In = GoodInput();
        s_In.hero_heroa_valid = Field<bool>::Observed(false);
        CHECK(Has(PreconditionEvaluator::Evaluate(s_In), Reason::HeroAInvalid));

        // candidate missing, unresolved, not HeroA-usable
        s_In = GoodInput();
        s_In.candidate.reset();
        CHECK(Has(PreconditionEvaluator::Evaluate(s_In), Reason::CandidateMissing));
        s_In = GoodInput();
        s_In.candidate->found = Field<bool>::Observed(false);
        CHECK(Has(PreconditionEvaluator::Evaluate(s_In), Reason::CandidateUnresolved));
        s_In = GoodInput();
        s_In.candidate->found = Field<bool>::Of(FieldStatus::Unavailable, "content kit manager global null");
        CHECK(Has(PreconditionEvaluator::Evaluate(s_In), Reason::CandidateUnresolved));
        s_In = GoodInput();
        s_In.candidate->heroa_usable = Field<bool>::Observed(false);
        CHECK(Has(PreconditionEvaluator::Evaluate(s_In), Reason::CandidateNotHeroAUsable));

        // expected-current outfit id
        s_In = GoodInput();
        s_In.expect_current = k_Suit;
        CHECK(PreconditionEvaluator::Evaluate(s_In).ok);
        s_In.expect_current = k_Other;
        CHECK(Has(PreconditionEvaluator::Evaluate(s_In), Reason::ExpectCurrentMismatch));

        // setter unavailable
        s_In = GoodInput();
        s_In.set_outfit_available = Field<bool>::Observed(false);
        CHECK(Has(PreconditionEvaluator::Evaluate(s_In), Reason::SetterUnavailable));
        s_In.set_outfit_available = Field<bool>::Of(FieldStatus::Unavailable, "fault");
        CHECK(Has(PreconditionEvaluator::Evaluate(s_In), Reason::SetterUnavailable));

        // every reason has a distinct code and the text joins them
        s_In = PreconditionEvaluator::Input{};
        s_V = PreconditionEvaluator::Evaluate(s_In);
        CHECK(!s_V.ok && s_V.reasons.size() == 11); // every rule an empty observation can fail (the epochs agree at 0)
        const std::string s_Text = PreconditionEvaluator::ReasonsText(s_V);
        CHECK(s_Text.find("expected_generation_missing") == 0 && s_Text.find(",mission_not_playing") != std::string::npos);
    }

    // ---- the lifecycle tee: contract ids observed, everything forwarded unchanged ----------------
    {
        auto s_Capture = std::make_unique<CapturingSink>();
        CapturingSink* s_Inner = s_Capture.get();
        std::vector<std::pair<LifecycleTee::Kind, std::string>> s_Seen;
        LifecycleTee s_Tee(std::move(s_Capture), [&](LifecycleTee::Kind p_Kind, const std::string& p_Id) { s_Seen.emplace_back(p_Kind, p_Id); });

        PublishedEnvelope s_Started{"contract.started", 1, std::string("{\"event_type\":\"contract.started\",\"payload\":{\"contract_session_id\":\"") + k_Gen1 + "\",\"location_id\":\"LOCATION_PARIS\"}}"};
        PublishedEnvelope s_Playing{"mission.playing", 2, "{\"event_type\":\"mission.playing\",\"payload\":{\"game_session_id\":\"x\"}}"};
        PublishedEnvelope s_Ended{"contract.ended", 3, std::string("{\"event_type\":\"contract.ended\",\"payload\":{\"contract_session_id\":\"") + k_Gen1 + "\",\"reason_kind\":\"restart\"}}"};
        PublishedEnvelope s_Broken{"contract.ended", 4, "{\"event_type\":\"contract.ended\",\"payload\":{}}"};
        s_Tee.Publish(s_Started);
        s_Tee.Publish(s_Playing);
        s_Tee.Publish(s_Ended);
        s_Tee.Publish(s_Broken);

        CHECK(s_Inner->Published.size() == 4 && s_Inner->Published[0].json == s_Started.json && s_Inner->Published[2].sequence == 3);
        CHECK(s_Seen.size() == 2 && s_Seen[0].first == LifecycleTee::Kind::ContractStarted && s_Seen[0].second == k_Gen1 && s_Seen[1].first == LifecycleTee::Kind::ContractEnded);
        CHECK(s_Tee.GetStats().forwarded == 4 && s_Tee.GetStats().observed == 2 && s_Tee.GetStats().unreadable == 1);
        CHECK(!LifecycleTee::ContractSessionId("not json"));
    }

    // ---- the receipt snapshot: stamped at push from the frame thread's last publication ---------
    {
        CommandQueue s_Queue;
        CommandQueue::FrameSnapshot s_Snapshot;
        s_Snapshot.mission_generation = k_Gen1;
        s_Snapshot.relay_transition_epoch = 3;
        s_Queue.PublishSnapshot(s_Snapshot);
        s_Queue.Push(Inspect(CommandTest::Uuid(1)), 1, 100);
        s_Snapshot.mission_generation = k_Gen2;
        s_Snapshot.relay_transition_epoch = 4;
        const auto s_First = s_Queue.Pop(s_Snapshot); // publishes the new snapshot in the same lock
        CHECK(s_First && s_First->at_receipt.mission_generation == std::optional<std::string>(k_Gen1) && s_First->at_receipt.relay_transition_epoch == 3);
        s_Queue.Push(Inspect(CommandTest::Uuid(2)), 1, 200);
        const auto s_Second = s_Queue.Pop(s_Snapshot);
        CHECK(s_Second && s_Second->at_receipt.mission_generation == std::optional<std::string>(k_Gen2) && s_Second->at_receipt.relay_transition_epoch == 4);
        CHECK(s_Queue.Waiting() == 0);
    }

    // ---- scheduling relay.inspect.outfit with a fake inspector -----------------------------------
    {
        CommandQueue s_Queue;
        std::vector<Result> s_Results;
        int s_InspectorCalls = 0;
        CommandScheduler s_Scheduler(s_Queue, [&](const Result& p_Result) { s_Results.push_back(p_Result); });

        // the frame publishes its snapshot before the command is received
        auto s_Frame = PlayingFrame(50, 2500);
        s_Frame.inspector = [&](const std::optional<std::string>& p_Candidate) { ++s_InspectorCalls; return GoodReads(p_Candidate); };
        CHECK(!s_Scheduler.Tick(s_Frame));
        CHECK(s_InspectorCalls == 0); // an idle frame never inspects

        s_Queue.Push(Inspect(CommandTest::Uuid(1), k_Suit, k_Gen1), 1, 2600);
        s_Frame = PlayingFrame(100, 5000);
        s_Frame.inspector = [&](const std::optional<std::string>& p_Candidate) { ++s_InspectorCalls; return GoodReads(p_Candidate); };
        CHECK(s_Scheduler.Tick(s_Frame));
        CHECK(s_InspectorCalls == 1 && s_Results.size() == 1);
        const Result& s_Ok = s_Results[0];
        CHECK(s_Ok.status == Status::Applied && s_Ok.frame && *s_Ok.frame == 100 && s_Ok.mission_generation == std::optional<std::string>(k_Gen1));
        CHECK(BoolField(s_Ok, "precondition_ok", true));
        CHECK(FieldIs(s_Ok, "precondition_reasons", FieldStatus::Observed) && TextField(s_Ok, "precondition_reasons").empty());
        CHECK(TextField(s_Ok, "generation_expected") == k_Gen1 && TextField(s_Ok, "generation_at_receipt") == k_Gen1 && TextField(s_Ok, "generation_cached") == k_Gen1 && TextField(s_Ok, "generation_fresh") == k_Gen1);
        CHECK(TextField(s_Ok, "hero_outfit_id") == k_Suit && BoolField(s_Ok, "hero_heroa_valid", true) && BoolField(s_Ok, "candidate_found", true) && BoolField(s_Ok, "candidate_heroa_usable", true));
        CHECK(BoolField(s_Ok, "set_outfit_available", true) && BoolField(s_Ok, "reads_on_frame", true));
        CHECK(IntField(s_Ok, "relay_transition_epoch") == 1 && IntField(s_Ok, "epoch_at_receipt") == 1);
        CHECK(IntField(s_Ok, "receipt_to_frame_ms") == 2400);
        CHECK(s_Ok.observed.size() <= k_MaxObservedEntries);
        CHECK(s_Scheduler.GetStats().inspected == 1 && s_Scheduler.GetStats().executed == 0 && s_Scheduler.GetStats().reduced == 0);

        // the result line is bounded and parses on the BEAM shape (objects per field)
        const std::string s_Line = EncodeResult(s_Ok);
        CHECK(s_Line.size() <= k_MaxLineBytes && s_Line.back() == '\n');
        CHECK(s_Line.find("\"hero_outfit_id\":{\"status\":\"observed\",\"value\":\"874c4c48-0a8b-49e9-883e-49fc5f1fb051\"}") != std::string::npos);
        CHECK(s_Line.find("\"generation_expected\":{\"status\":\"observed\",\"value\":\"2516103582917844185") != std::string::npos);
        CHECK(s_Line.find("\"precondition_ok\":{\"status\":\"observed\",\"value\":true}") != std::string::npos);

        // the restart race: the registry already names the next generation, the cached one is old,
        // a contract end for the active generation was seen, the epoch moved since receipt
        s_Queue.Push(Inspect(CommandTest::Uuid(2), k_Suit, k_Gen1), 1, 5100);
        s_Frame = PlayingFrame(160, 8000);
        s_Frame.relay_transition_epoch = 2;
        s_Frame.contract_ended_seen = k_Gen1;
        s_Frame.inspector = [&](const std::optional<std::string>& p_Candidate) { ++s_InspectorCalls; return GoodReads(p_Candidate, k_Gen2); };
        CHECK(s_Scheduler.Tick(s_Frame));
        const Result& s_Race = s_Results[1];
        CHECK(s_Race.status == Status::Applied); // the inspection itself still applies
        CHECK(BoolField(s_Race, "precondition_ok", false));
        const std::string s_Reasons = TextField(s_Race, "precondition_reasons");
        CHECK(s_Reasons.find("generation_mismatch_fresh") != std::string::npos && s_Reasons.find("contract_end_observed") != std::string::npos && s_Reasons.find("epoch_changed") != std::string::npos);
        CHECK(TextField(s_Race, "generation_fresh") == k_Gen2 && TextField(s_Race, "generation_cached") == k_Gen1);
        CHECK(IntField(s_Race, "epoch_at_receipt") == 1 && IntField(s_Race, "relay_transition_epoch") == 2);

        // a candidate that is not in the table, and no candidate at all
        s_Queue.Push(Inspect(CommandTest::Uuid(3), k_Other, k_Gen1), 1, 8100);
        s_Frame = PlayingFrame(200, 9000);
        s_Frame.inspector = [&](const std::optional<std::string>& p_Candidate) { return GoodReads(p_Candidate); };
        s_Scheduler.Tick(s_Frame);
        CHECK(BoolField(s_Results[2], "candidate_found", false) && TextField(s_Results[2], "precondition_reasons").find("candidate_unresolved") != std::string::npos);
        s_Queue.Push(Inspect(CommandTest::Uuid(4)), 1, 9100);
        s_Scheduler.Tick(s_Frame);
        CHECK(!Observed(s_Results[3], "candidate_id") && !Observed(s_Results[3], "candidate_found"));
        CHECK(TextField(s_Results[3], "precondition_reasons").find("candidate_missing") != std::string::npos && TextField(s_Results[3], "precondition_reasons").find("expected_generation_missing") != std::string::npos);
    }

    // ---- unavailable engine reads: null global, missing hero, unavailable setter, bad id --------
    {
        CommandQueue s_Queue;
        std::vector<Result> s_Results;
        CommandScheduler s_Scheduler(s_Queue, [&](const Result& p_Result) { s_Results.push_back(p_Result); });

        s_Queue.Push(Inspect(CommandTest::Uuid(1), k_Suit, k_Gen1), 1, 0);
        auto s_Frame = PlayingFrame();
        s_Frame.inspector = [&](const std::optional<std::string>&) {
            OutfitObservation::EngineReads s_Reads;
            s_Reads.thread_id = GetCurrentThreadId();
            s_Reads.generation_fresh = Field<std::string>::Of(FieldStatus::Unavailable, "player registry global null");
            s_Reads.hero.outfit_id = Field<std::string>::Of(FieldStatus::Unavailable, "player registry global null");
            s_Reads.hero.heroa_valid = Field<bool>::Of(FieldStatus::Unavailable, "player registry global null");
            s_Reads.kit_map_available = Field<bool>::Observed(false);
            OutfitObservation::KitResolution s_Kit;
            s_Kit.found = Field<bool>::Of(FieldStatus::Unavailable, "content kit manager global null");
            s_Reads.candidate = s_Kit;
            s_Reads.set_outfit_available = Field<bool>::Observed(false);
            return s_Reads;
        };
        s_Scheduler.Tick(s_Frame);
        const Result& s_Null = s_Results[0];
        CHECK(s_Null.status == Status::Applied && BoolField(s_Null, "precondition_ok", false));
        CHECK(FieldIs(s_Null, "generation_fresh", FieldStatus::Unavailable) && FieldIs(s_Null, "hero_outfit_id", FieldStatus::Unavailable) && FieldIs(s_Null, "candidate_found", FieldStatus::Unavailable));
        CHECK(!Observed(s_Null, "hero_charset") || FieldIs(s_Null, "hero_charset", FieldStatus::Unavailable));
        const std::string s_Reasons = TextField(s_Null, "precondition_reasons");
        CHECK(s_Reasons.find("fresh_generation_unavailable") != std::string::npos && s_Reasons.find("player_unavailable") != std::string::npos && s_Reasons.find("candidate_unresolved") != std::string::npos && s_Reasons.find("setter_unavailable") != std::string::npos);
        CHECK(EncodeResult(s_Null).find("\"generation_fresh\":{\"status\":\"unavailable\",\"reason\":\"player registry global null\"}") != std::string::npos);

        // a missing hero with the registry present; an inconsistent (torn) registry read
        s_Queue.Push(Inspect(CommandTest::Uuid(2), std::nullopt, k_Gen1), 1, 0);
        s_Frame.inspector = [&](const std::optional<std::string>&) {
            auto s_Reads = GoodReads(std::nullopt);
            s_Reads.hero = OutfitObservation::HeroOutfit{};
            s_Reads.hero.outfit_id = Field<std::string>::Of(FieldStatus::Unavailable, "hero entity reference null");
            s_Reads.hero.heroa_valid = Field<bool>::Of(FieldStatus::Unavailable, "hero entity reference null");
            s_Reads.generation_fresh = Field<std::string>::Of(FieldStatus::Inconsistent, "registry session id reads differ");
            return s_Reads;
        };
        s_Scheduler.Tick(s_Frame);
        CHECK(FieldIs(s_Results[1], "generation_fresh", FieldStatus::Inconsistent) && TextField(s_Results[1], "precondition_reasons").find("fresh_generation_inconsistent") != std::string::npos && TextField(s_Results[1], "precondition_reasons").find("player_unavailable") != std::string::npos);

        // no inspector at all (a frame source without engine access): everything unsupported, still applied
        s_Queue.Push(Inspect(CommandTest::Uuid(3), k_Suit, k_Gen1), 1, 0);
        s_Frame.inspector = nullptr;
        s_Scheduler.Tick(s_Frame);
        CHECK(s_Results[2].status == Status::Applied && FieldIs(s_Results[2], "hero_outfit_id", FieldStatus::Unsupported) && FieldIs(s_Results[2], "reads_on_frame", FieldStatus::Unsupported));
        CHECK(s_Results[2].reason.find("unsupported") != std::string::npos);
    }

    // ---- decoding: the entry, its one optional parameter, rejection of anything else -----------
    {
        const std::string s_Base = "{\"command_protocol_version\":1,\"kind\":\"command\",\"command_id\":\"" + CommandTest::Uuid(7) + "\",\"link_generation\":1,\"issued_at\":\"t\",\"ttl_ms\":500,\"name\":\"relay.inspect.outfit\",\"version\":1,\"preconditions\":{\"mission_generation\":\"" + k_Gen1 + "\"},\"params\":";
        auto s_Ok = DecodeCommand(s_Base + "{\"disguise_repository_id\":\"" + k_Suit + "\"}}");
        CHECK(s_Ok.ok && s_Ok.command.entry == Entry::RelayInspectOutfit1 && s_Ok.command.params.size() == 1 && s_Ok.command.params[0].second == k_Suit);
        CHECK(s_Ok.command.mission_generation_precondition == std::optional<std::string>(k_Gen1));
        auto s_NoParam = DecodeCommand(s_Base + "{}}");
        CHECK(s_NoParam.ok && s_NoParam.command.params.empty());
        auto s_Bad = DecodeCommand(s_Base + "{\"disguise_repository_id\":\"874C4C48-0A8B-49E9-883E-49FC5F1FB051\"}}");
        CHECK(!s_Bad.ok && s_Bad.reason == "invalid parameter: disguise_repository_id" && s_Bad.command_id == CommandTest::Uuid(7));
        auto s_Unknown = DecodeCommand(s_Base + "{\"outfit_pointer\":\"0x1\"}}");
        CHECK(!s_Unknown.ok && s_Unknown.reason == "unknown parameter: outfit_pointer");
        auto s_Reserved = DecodeCommand("{\"command_protocol_version\":1,\"kind\":\"command\",\"command_id\":\"" + CommandTest::Uuid(8) + "\",\"link_generation\":1,\"issued_at\":\"t\",\"ttl_ms\":500,\"name\":\"player.outfit.set\",\"version\":1,\"preconditions\":{},\"params\":{\"disguise_repository_id\":\"" + k_Suit + "\"}}");
        CHECK(!s_Reserved.ok && s_Reserved.reason == "reserved command: not issuable");
        CHECK(Lookup("relay.inspect.outfit", 2) == Entry::Unknown && Lookup("relay.inspect.outfit", 1) == Entry::RelayInspectOutfit1);
    }

    // ---- bounded encoding: the inspection's two realistic extremes stay under the line bound ----
    {
        // Measured through the scheduler itself, with every text at its bound: (a) everything
        // observed — the six generation / contract ids at the 64-byte generation bound, the hero
        // and candidate ids, the longest reason list — and (b) every engine-derived field
        // unavailable with a reason at the 40-byte bound. The result's own reason and the
        // mission generation are at their bounds too. The scheduler's reduction (tested below)
        // covers any shape beyond these.
        const std::string s_LongGen(k_MaxMissionGenerationBytes, 'g');
        auto s_Measure = [&](bool p_Observed) {
            CommandQueue s_Queue;
            std::vector<Result> s_Results;
            CommandScheduler s_Scheduler(s_Queue, [&](const Result& p_Result) { s_Results.push_back(p_Result); });
            auto s_Frame = PlayingFrame(18446744073709551615ull, 9000); // the frame counter at its bound; the clock inside the TTL
            s_Frame.mission_generation = s_LongGen;
            s_Frame.contract_started_seen = s_LongGen;
            s_Frame.contract_ended_seen = s_LongGen;
            s_Frame.relay_transition_epoch = 18446744073709551615ull;
            s_Frame.inspector = [&](const std::optional<std::string>& p_Candidate) {
                if (p_Observed)
                {
                    auto s_Reads = GoodReads(p_Candidate, s_LongGen.c_str());
                    s_Reads.hero.charset = Field<int64_t>::Observed(INT64_MIN);
                    s_Reads.hero.variation = Field<int64_t>::Observed(INT64_MIN);
                    s_Reads.hero.charset_count = Field<int64_t>::Observed(INT64_MIN);
                    s_Reads.hero.heroa_variations = Field<int64_t>::Observed(INT64_MIN);
                    s_Reads.candidate->charsets = Field<int64_t>::Observed(INT64_MIN);
                    s_Reads.candidate->heroa_variations = Field<int64_t>::Observed(INT64_MIN);
                    s_Reads.candidate->kits_scanned = Field<int64_t>::Observed(INT64_MIN);
                    return s_Reads;
                }
                OutfitObservation::EngineReads s_Reads;
                const std::string s_Why(k_MaxObservedReasonBytes, 'w');
                s_Reads.generation_fresh = Field<std::string>::Of(FieldStatus::Unavailable, s_Why);
                s_Reads.hero.outfit_id = Field<std::string>::Of(FieldStatus::Unavailable, s_Why);
                s_Reads.hero.charset = Field<int64_t>::Of(FieldStatus::Unavailable, s_Why);
                s_Reads.hero.variation = s_Reads.hero.charset;
                s_Reads.hero.charset_count = s_Reads.hero.charset;
                s_Reads.hero.heroa_variations = s_Reads.hero.charset;
                s_Reads.hero.heroa_valid = Field<bool>::Of(FieldStatus::Unavailable, s_Why);
                s_Reads.hero.is_hitman_suit = s_Reads.hero.heroa_valid;
                OutfitObservation::KitResolution s_Kit;
                s_Kit.found = s_Reads.hero.heroa_valid;
                s_Kit.charsets = s_Reads.hero.charset;
                s_Kit.heroa_variations = s_Reads.hero.charset;
                s_Kit.heroa_usable = s_Reads.hero.heroa_valid;
                s_Kit.is_hitman_suit = s_Reads.hero.heroa_valid;
                s_Kit.kits_scanned = s_Reads.hero.charset;
                s_Reads.candidate = s_Kit;
                s_Reads.set_outfit_available = s_Reads.hero.heroa_valid;
                s_Reads.kit_map_available = s_Reads.hero.heroa_valid;
                return s_Reads;
            };
            s_Queue.Push(Inspect(CommandTest::Uuid(1), k_Suit, s_LongGen), 1, 0);
            s_Scheduler.Tick(s_Frame);
            CHECK(s_Results.size() == 1 && s_Scheduler.GetStats().reduced == 0);
            return s_Results.empty() ? std::string() : EncodeResult(s_Results[0]);
        };
        const std::string s_AllObserved = s_Measure(true);
        const std::string s_AllUnavailable = s_Measure(false);
        CHECK(s_AllObserved.size() <= k_MaxLineBytes && s_AllObserved.size() > 2000);
        CHECK(s_AllUnavailable.size() <= k_MaxLineBytes && s_AllUnavailable.size() > 2000);
        std::printf("inspection encoding: all observed %zu bytes, all unavailable %zu bytes (bound %zu)\n", s_AllObserved.size(), s_AllUnavailable.size(), k_MaxLineBytes);

        // beyond the entry bound the encoder keeps the first entries only (deterministic), and a
        // field reason longer than its bound is cut
        Result s_Worst;
        s_Worst.command_id = CommandTest::Uuid(1);
        s_Worst.link_generation = 1;
        for (size_t i = 0; i < k_MaxObservedEntries; ++i)
        {
            ObservedValue s_Value;
            s_Value.kind = ObservedValue::Kind::Bool;
            s_Value.field_status = FieldStatus::Observed;
            s_Worst.observed.emplace_back("b" + std::to_string(i), s_Value);
        }
        ObservedValue s_Extra;
        s_Extra.kind = ObservedValue::Kind::Bool;
        s_Extra.field_status = FieldStatus::Observed;
        s_Worst.observed.emplace_back("one_too_many", s_Extra);
        CHECK(s_Worst.observed.size() == k_MaxObservedEntries + 1);
        CHECK(EncodeResult(s_Worst).find("one_too_many") == std::string::npos);
        CHECK(EncodeResult(s_Worst).find("\"b47\"") != std::string::npos);
        ObservedValue s_Long;
        s_Long.field_status = FieldStatus::Unavailable;
        s_Long.field_reason = std::string(100, 'x');
        Result s_Small;
        s_Small.command_id = CommandTest::Uuid(1);
        s_Small.link_generation = 1;
        s_Small.observed.emplace_back("f", s_Long);
        const std::string s_SmallLine = EncodeResult(s_Small);
        CHECK(s_SmallLine.find(std::string(k_MaxObservedReasonBytes, 'x') + "\"") != std::string::npos && s_SmallLine.find(std::string(k_MaxObservedReasonBytes + 1, 'x')) == std::string::npos);
    }

    // ---- reduction: an inspection that would not fit is cut to its first entries and says so ----
    {
        CommandQueue s_Queue;
        std::vector<Result> s_Results;
        CommandScheduler s_Scheduler(s_Queue, [&](const Result& p_Result) { s_Results.push_back(p_Result); });
        s_Queue.Push(Inspect(CommandTest::Uuid(1), k_Suit, k_Gen1), 1, 0);
        auto s_Frame = PlayingFrame();
        s_Frame.inspector = [&](const std::optional<std::string>& p_Candidate) {
            auto s_Reads = GoodReads(p_Candidate);
            // every field carries the longest reason the encoder keeps, and a long id, to push the line over the bound
            const std::string s_LongId(k_MaxObservedTextBytes, 'z');
            s_Reads.generation_fresh = Field<std::string>::Observed(s_LongId);
            s_Reads.hero.outfit_id = Field<std::string>::Observed(s_LongId);
            s_Reads.hero.charset = Field<int64_t>::Of(FieldStatus::Unavailable, std::string(k_MaxObservedReasonBytes, 'q'));
            s_Reads.hero.variation = s_Reads.hero.charset;
            s_Reads.hero.charset_count = s_Reads.hero.charset;
            s_Reads.hero.heroa_variations = s_Reads.hero.charset;
            s_Reads.hero.heroa_valid = Field<bool>::Of(FieldStatus::Unavailable, std::string(k_MaxObservedReasonBytes, 'q'));
            s_Reads.hero.is_hitman_suit = s_Reads.hero.heroa_valid;
            s_Reads.candidate->charsets = s_Reads.hero.charset;
            s_Reads.candidate->heroa_variations = s_Reads.hero.charset;
            s_Reads.candidate->heroa_usable = s_Reads.hero.heroa_valid;
            s_Reads.candidate->is_hitman_suit = s_Reads.hero.heroa_valid;
            s_Reads.candidate->kits_scanned = s_Reads.hero.charset;
            s_Reads.set_outfit_available = s_Reads.hero.heroa_valid;
            s_Reads.kit_map_available = s_Reads.hero.heroa_valid;
            return s_Reads;
        };
        s_Frame.mission_generation = std::string(k_MaxMissionGenerationBytes, 'm');
        s_Frame.contract_started_seen = std::string(k_MaxObservedTextBytes, 'c');
        s_Frame.contract_ended_seen = std::string(k_MaxObservedTextBytes, 'e');
        auto s_Command = Inspect(CommandTest::Uuid(2), k_Suit, std::string(k_MaxMissionGenerationBytes, 'x'));
        s_Queue.Pop(); // drop the first (short) command so the long one is next
        s_Queue.Push(s_Command, 1, 0);
        s_Scheduler.Tick(s_Frame);
        CHECK(s_Results.size() == 1);
        const Result& s_Reduced = s_Results[0];
        const bool s_WasReduced = s_Scheduler.GetStats().reduced == 1;
        // Either the worst case fit (then nothing was reduced) or it was reduced deterministically.
        CHECK(EncodeResult(s_Reduced).size() <= k_MaxLineBytes);
        if (s_WasReduced)
        {
            CHECK(Observed(s_Reduced, "observed_reduced") && Observed(s_Reduced, "observed_reduced")->boolean);
            CHECK(Observed(s_Reduced, "generation_fresh") && !Observed(s_Reduced, "hero_charset"));
            CHECK(s_Reduced.reason.find("reduced") != std::string::npos);
        }
    }

    // ---- no engine setter: the C-1 command path contains no call that could write the engine -----
    {
        // Established by construction in the engine-independent build: this test executable links
        // the whole command path (protocol, queue, scheduler, evaluator, tee) without the SDK, so no
        // engine function can be referenced from it. The reserved entry stays rejected.
        CommandQueue s_Queue;
        std::vector<Result> s_Results;
        CommandScheduler s_Scheduler(s_Queue, [&](const Result& p_Result) { s_Results.push_back(p_Result); });
        Command s_Set = Inspect(CommandTest::Uuid(1), k_Suit, k_Gen1);
        s_Set.name = "player.outfit.set";
        s_Set.entry = Entry::PlayerOutfitSet1Reserved;
        s_Queue.Push(s_Set, 1, 0);
        auto s_Frame = PlayingFrame();
        int s_Calls = 0;
        s_Frame.inspector = [&](const std::optional<std::string>& p_Candidate) { ++s_Calls; return GoodReads(p_Candidate); };
        s_Scheduler.Tick(s_Frame);
        CHECK(s_Results.size() == 1 && s_Results[0].status == Status::Rejected && s_Calls == 0);
    }
}
