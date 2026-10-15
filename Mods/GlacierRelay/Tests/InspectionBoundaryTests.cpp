// M6-B1 hardening tests: the three read-only inspection boundary corrections, each exercised on the
// pure rule the DLL applies and on the scheduler's encoding of a FAKE read set that reports the
// corresponding engine answer. Correction A: the blueprint-factory index bounds (a valid count
// before use, an index inside [0, count) before the one sub-entity access, a negative index kept
// distinct from a non-negative out-of-bounds one, every refusal a typed observation). Fake values
// only: no engine pointer is constructed, valid or invalid; no memory is written; no setter exists
// on this path.
#include "TestHarness.h"

#include <cstring>
#include <string>
#include <vector>

#include <Windows.h>

#include "CommandScheduler.h"
#include "PlayerObservation.h"
#include "TestListener.h"

using namespace CommandProtocol;
using OutfitObservation::Field;
using PlayerObservation::EntityIdentity;
using PlayerObservation::EntityQuery;
using PlayerObservation::EntityReads;
using PlayerObservation::IndexCheck;
using PlayerObservation::LookupFacts;
using PlayerObservation::RoundTrip;

namespace
{
    constexpr const char* k_Gen1 = "2516103582917844185-a105ec5f-3a75-46a3-8ecd-e72fd15b176c";
    constexpr const char* k_HitmenBrick = "007c57a6ea581f8c"; // a FAKE stand-in for the historical brick id
    constexpr const char* k_Agent48 = "feede715906f747f";
    constexpr const char* k_HeroId = "feed9a0d70d546f4";      // a FAKE authored id for the local hero
    constexpr const char* k_HeroBrick = "0012e5a7b1c9d3ee";   // a FAKE loaded brick that owns the hero

    Command EntityCommand(const std::string& p_Id, std::optional<std::string> p_Brick = std::nullopt, std::optional<std::string> p_Entity = std::nullopt)
    {
        Command s_Command;
        s_Command.command_id = p_Id;
        s_Command.link_generation = 1;
        s_Command.ttl_ms = k_MaxTtlMs;
        s_Command.name = "relay.inspect.entity";
        s_Command.version = 1;
        s_Command.entry = Entry::RelayInspectEntity1;
        if (p_Brick)
            s_Command.params.emplace_back("brick", *p_Brick);
        if (p_Entity)
            s_Command.params.emplace_back("entity_id", *p_Entity);
        return s_Command;
    }

    CommandScheduler::FrameObservation Frame(uint64_t p_Frame, uint64_t p_NowMs, bool p_Playing)
    {
        CommandScheduler::FrameObservation s_Frame;
        s_Frame.frame = p_Frame;
        s_Frame.now_mono_ms = p_NowMs;
        s_Frame.native_at = "t";
        s_Frame.thread_id = GetCurrentThreadId();
        s_Frame.scene_available = true;
        s_Frame.scene.available = true;
        s_Frame.scene.scene_resource = p_Playing ? "assembly:/_PRO/Scenes/Missions/Paris/_Scene_FashionShowHit_01.entity" : "assembly:/_PRO/Scenes/Frontend/MainMenu.entity";
        s_Frame.scene.scene_loaded = true;
        s_Frame.scene.loading_stage = 8;
        s_Frame.scene.scene_type = p_Playing ? "mission" : "";
        s_Frame.scene.codename_hint = p_Playing ? "Peacock" : "";
        s_Frame.mission_playing = p_Playing;
        if (p_Playing)
            s_Frame.mission_generation = k_Gen1;
        s_Frame.link_generation = 1;
        s_Frame.adapter_instance_id = CommandTest::Uuid(99);
        s_Frame.relay_transition_epoch = 1;
        s_Frame.previous_frame_playing = p_Playing;
        return s_Frame;
    }

    EntityIdentity HeroIdentity(const char* p_EntityId = k_HeroId, const char* p_Brick = k_HeroBrick)
    {
        EntityIdentity s_Identity;
        s_Identity.resolved = Field<bool>::Observed(true);
        s_Identity.entity_id = Field<std::string>::Observed(p_EntityId);
        s_Identity.entity_flags = Field<int64_t>::Observed(7);
        s_Identity.activated = Field<bool>::Observed(true);
        s_Identity.predeleted = Field<bool>::Observed(false);
        s_Identity.owning_brick = Field<std::string>::Observed(p_Brick);
        s_Identity.character_id = Field<std::string>::Observed("787c1ff2-467c-421d-98f4-6bbb92e5fc99");
        s_Identity.initial_outfit_id = Field<std::string>::Observed("874c4c48-0a8b-49e9-883e-49fc5f1fb051");
        s_Identity.invincible = Field<bool>::Observed(false);
        s_Identity.x = Field<double>::Observed(-0.035);
        s_Identity.y = Field<double>::Observed(-3.832);
        s_Identity.z = Field<double>::Observed(-0.413);
        s_Identity.transform_dirty = Field<bool>::Observed(false);
        s_Identity.room_behaviour = Field<int64_t>::Observed(1);
        return s_Identity;
    }

    // The lookup facts a FAKE factory would yield for a scripted count and index, following exactly
    // the steps the Glacier-facing reader takes (SceneObservation's Lookup): the count observed as
    // read; an invalid count stops before the index; a negative index is "not a sub-entity"; an
    // out-of-range index is an inconsistent factory answer; only an in-range index resolves.
    LookupFacts ScriptedLookup(int64_t p_Count, std::optional<int64_t> p_Index, bool p_Same = true)
    {
        LookupFacts s_Facts;
        s_Facts.owner_found = Field<bool>::Observed(true);
        s_Facts.factory_available = Field<bool>::Observed(true);
        s_Facts.factory_template = Field<bool>::Observed(true);
        s_Facts.subentity_count = Field<int64_t>::Observed(p_Count);

        if (!PlayerObservation::SubEntityCountValid(p_Count))
        {
            s_Facts.index = Field<int64_t>::Of(FieldStatus::Unavailable, "count invalid; index not looked up");
            s_Facts.index_check = Field<std::string>::Observed(PlayerObservation::IndexCheckText(IndexCheck::CountInvalid));
            s_Facts.resolved = Field<bool>::Of(FieldStatus::Inconsistent, "sub-entity count invalid");
            s_Facts.same_entity = Field<bool>::Of(FieldStatus::Inconsistent, "sub-entity count invalid");
            return s_Facts;
        }

        const int64_t s_Index = p_Index.value_or(-1);
        s_Facts.index = Field<int64_t>::Observed(s_Index);
        const IndexCheck s_Check = PlayerObservation::CheckSubEntityIndex(p_Count, s_Index);
        s_Facts.index_check = Field<std::string>::Observed(PlayerObservation::IndexCheckText(s_Check));
        if (s_Check == IndexCheck::Negative)
        {
            s_Facts.resolved = Field<bool>::Of(FieldStatus::Unavailable, "not a sub-entity of that factory");
            s_Facts.same_entity = Field<bool>::Of(FieldStatus::Unavailable, "not a sub-entity of that factory");
            return s_Facts;
        }
        if (s_Check != IndexCheck::InRange)
        {
            s_Facts.resolved = Field<bool>::Of(FieldStatus::Inconsistent, "index outside the sub-entity count");
            s_Facts.same_entity = Field<bool>::Of(FieldStatus::Inconsistent, "index outside the sub-entity count");
            return s_Facts;
        }

        s_Facts.resolved = Field<bool>::Observed(true);
        s_Facts.same_entity = Field<bool>::Observed(p_Same);
        return s_Facts;
    }

    std::vector<std::string> FakeBrickIds(size_t p_Count)
    {
        std::vector<std::string> s_Ids;
        for (size_t i = 0; i < p_Count; ++i)
        {
            char s_Buffer[17] = {};
            std::snprintf(s_Buffer, sizeof(s_Buffer), "00%014zx", i + 1);
            s_Ids.emplace_back(s_Buffer);
        }
        return s_Ids;
    }

    // A stable single-player entity read set (23 FAKE bricks, the hero owned by one of them, the
    // candidate brick absent) whose round trip is the scripted lookup.
    EntityReads EntityWithRoundTrip(const LookupFacts& p_Trip, size_t p_Bricks = 23)
    {
        EntityReads s_Reads;
        s_Reads.thread_id = GetCurrentThreadId();
        s_Reads.bricks.count = Field<int64_t>::Observed(static_cast<int64_t>(p_Bricks));
        const auto s_All = FakeBrickIds(p_Bricks);
        for (size_t i = 0; i < s_All.size() && i < PlayerObservation::k_MaxBrickIds; ++i)
            s_Reads.bricks.ids.push_back(s_All[i]);
        s_Reads.bricks.truncated = Field<bool>::Observed(p_Bricks > PlayerObservation::k_MaxBrickIds);
        s_Reads.hero = HeroIdentity();
        s_Reads.round_trip = p_Trip;
        s_Reads.round_trip_outcome = PlayerObservation::ClassifyRoundTrip(s_Reads.round_trip);

        PlayerObservation::CandidateReads s_Candidate;
        s_Candidate.brick_id = k_HitmenBrick;
        s_Candidate.entity_id = k_Agent48;
        s_Candidate.brick_present = Field<bool>::Observed(false);
        PlayerObservation::MarkLookup(s_Candidate.lookup, FieldStatus::Unavailable, "candidate brick not loaded");
        PlayerObservation::MarkIdentity(s_Candidate.identity, FieldStatus::Unavailable, "candidate brick not loaded");
        s_Candidate.hitman5_resolved = Field<bool>::Of(FieldStatus::Unavailable, "candidate brick not loaded");
        s_Candidate.spatial_resolved = Field<bool>::Of(FieldStatus::Unavailable, "candidate brick not loaded");
        s_Reads.candidate = s_Candidate;
        return s_Reads;
    }

    // The same read set with the candidate brick present and its lookup scripted.
    EntityReads WithCandidateLookup(EntityReads p_Reads, const LookupFacts& p_Lookup)
    {
        auto& s_Candidate = *p_Reads.candidate;
        s_Candidate.brick_present = Field<bool>::Observed(true);
        s_Candidate.lookup = p_Lookup;
        if (p_Lookup.resolved.observed() && *p_Lookup.resolved.value)
        {
            s_Candidate.identity = HeroIdentity(k_Agent48, k_HitmenBrick);
            s_Candidate.hitman5_resolved = Field<bool>::Observed(true);
            s_Candidate.spatial_resolved = Field<bool>::Observed(true);
        }
        else
        {
            PlayerObservation::MarkIdentity(s_Candidate.identity, FieldStatus::Unavailable, "candidate entity not resolved");
            s_Candidate.hitman5_resolved = Field<bool>::Of(FieldStatus::Unavailable, "candidate entity not resolved");
            s_Candidate.spatial_resolved = Field<bool>::Of(FieldStatus::Unavailable, "candidate entity not resolved");
        }
        return p_Reads;
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

    int64_t IntField(const Result& p_Result, const char* p_Key)
    {
        const auto* s_Value = Observed(p_Result, p_Key);
        return s_Value && s_Value->kind == ObservedValue::Kind::Integer ? s_Value->integer : INT64_MIN;
    }

    std::string Reason(const Result& p_Result, const char* p_Key)
    {
        const auto* s_Value = Observed(p_Result, p_Key);
        return s_Value ? s_Value->field_reason : std::string();
    }

    Result RunEntity(const CommandScheduler::FrameObservation& p_Frame, CommandScheduler::EntityInspector p_Inspector, const Command& p_Command, CommandScheduler::Stats* p_Stats = nullptr)
    {
        CommandQueue s_Queue;
        std::vector<Result> s_Results;
        CommandScheduler s_Scheduler(s_Queue, [&](const Result& p_Result) { s_Results.push_back(p_Result); });
        s_Queue.Push(p_Command, 1, p_Frame.now_mono_ms > 10 ? p_Frame.now_mono_ms - 10 : 0);
        CommandScheduler::FrameObservation s_Frame = p_Frame;
        s_Frame.entity_inspector = std::move(p_Inspector);
        CHECK(s_Scheduler.Tick(s_Frame));
        CHECK(s_Results.size() == 1);
        if (p_Stats)
            *p_Stats = s_Scheduler.GetStats();
        return s_Results.empty() ? Result{} : s_Results[0];
    }
}

void RunInspectionBoundaryTests()
{
    // ==== Correction A: blueprint-factory index bounds ==============================================

    // ---- the pure rule: count validity and the index check vocabulary --------------------------------
    {
        CHECK(PlayerObservation::SubEntityCountValid(0));
        CHECK(PlayerObservation::SubEntityCountValid(1));
        CHECK(PlayerObservation::SubEntityCountValid(PlayerObservation::k_MaxSubEntityCount));
        CHECK(!PlayerObservation::SubEntityCountValid(-1));
        CHECK(!PlayerObservation::SubEntityCountValid(INT64_MIN));
        CHECK(!PlayerObservation::SubEntityCountValid(PlayerObservation::k_MaxSubEntityCount + 1));
        CHECK(!PlayerObservation::SubEntityCountValid(INT64_MAX));

        // zero count: only a negative index is coherent; index 0 is out of range
        CHECK(PlayerObservation::CheckSubEntityIndex(0, -1) == IndexCheck::Negative);
        CHECK(PlayerObservation::CheckSubEntityIndex(0, 0) == IndexCheck::OutOfRange);
        // negative count: invalid whatever the index would have been
        CHECK(PlayerObservation::CheckSubEntityIndex(-1, 0) == IndexCheck::CountInvalid);
        CHECK(PlayerObservation::CheckSubEntityIndex(-1, -1) == IndexCheck::CountInvalid);
        CHECK(PlayerObservation::CheckSubEntityIndex(INT64_MAX, 0) == IndexCheck::CountInvalid);
        // negative index: the factory's "not a sub-entity" answer, whatever its magnitude
        CHECK(PlayerObservation::CheckSubEntityIndex(2, -1) == IndexCheck::Negative);
        CHECK(PlayerObservation::CheckSubEntityIndex(2, INT32_MIN) == IndexCheck::Negative);
        // index equal to the count, and greater than it: out of range, never negative
        CHECK(PlayerObservation::CheckSubEntityIndex(2, 2) == IndexCheck::OutOfRange);
        CHECK(PlayerObservation::CheckSubEntityIndex(2, 3) == IndexCheck::OutOfRange);
        CHECK(PlayerObservation::CheckSubEntityIndex(2, INT32_MAX) == IndexCheck::OutOfRange);
        // the valid final index, and the first
        CHECK(PlayerObservation::CheckSubEntityIndex(2, 1) == IndexCheck::InRange);
        CHECK(PlayerObservation::CheckSubEntityIndex(2, 0) == IndexCheck::InRange);
        CHECK(PlayerObservation::CheckSubEntityIndex(1, 0) == IndexCheck::InRange);
        CHECK(PlayerObservation::CheckSubEntityIndex(PlayerObservation::k_MaxSubEntityCount, PlayerObservation::k_MaxSubEntityCount - 1) == IndexCheck::InRange);

        CHECK(std::string(PlayerObservation::IndexCheckText(IndexCheck::InRange)) == "in_range");
        CHECK(std::string(PlayerObservation::IndexCheckText(IndexCheck::Negative)) == "negative");
        CHECK(std::string(PlayerObservation::IndexCheckText(IndexCheck::OutOfRange)) == "out_of_range");
        CHECK(std::string(PlayerObservation::IndexCheckText(IndexCheck::CountInvalid)) == "count_invalid");
    }

    // ---- the round-trip classification with the check: existing outcomes keep their meaning ---------
    {
        CHECK(PlayerObservation::ClassifyRoundTrip(ScriptedLookup(2, 1, true)) == RoundTrip::SameEntity);
        CHECK(PlayerObservation::ClassifyRoundTrip(ScriptedLookup(2, 1, false)) == RoundTrip::DifferentEntity);
        CHECK(PlayerObservation::ClassifyRoundTrip(ScriptedLookup(2, 0, true)) == RoundTrip::SameEntity);
        CHECK(PlayerObservation::ClassifyRoundTrip(ScriptedLookup(2, -1)) == RoundTrip::HeroNotRepresentable);
        CHECK(PlayerObservation::ClassifyRoundTrip(ScriptedLookup(0, -1)) == RoundTrip::HeroNotRepresentable);
        // the new refusals are inconclusive (unsupported), never "different entity" and never "not representable"
        CHECK(PlayerObservation::ClassifyRoundTrip(ScriptedLookup(2, 2)) == RoundTrip::Unsupported);
        CHECK(PlayerObservation::ClassifyRoundTrip(ScriptedLookup(2, 5)) == RoundTrip::Unsupported);
        CHECK(PlayerObservation::ClassifyRoundTrip(ScriptedLookup(0, 0)) == RoundTrip::Unsupported);
        CHECK(PlayerObservation::ClassifyRoundTrip(ScriptedLookup(-1, 0)) == RoundTrip::Unsupported);
        CHECK(PlayerObservation::ClassifyRoundTrip(ScriptedLookup(PlayerObservation::k_MaxSubEntityCount + 1, 0)) == RoundTrip::Unsupported);

        // a lookup that claims a resolved entity without an in-range check is not demonstrably valid
        LookupFacts s_Unchecked = ScriptedLookup(2, 1, false);
        s_Unchecked.index_check = Field<std::string>::Of(FieldStatus::Unavailable, "not checked");
        CHECK(PlayerObservation::ClassifyRoundTrip(s_Unchecked) == RoundTrip::Unsupported);
        LookupFacts s_Lying = ScriptedLookup(2, 1, false);
        s_Lying.index_check = Field<std::string>::Observed("out_of_range");
        CHECK(PlayerObservation::ClassifyRoundTrip(s_Lying) == RoundTrip::Unsupported);

        // the marked (unread) lookup carries the check as marked too
        LookupFacts s_Marked;
        PlayerObservation::MarkLookup(s_Marked, FieldStatus::Unavailable, "hero entity reference null");
        CHECK(s_Marked.index_check.status == FieldStatus::Unavailable && s_Marked.index_check.reason == "hero entity reference null");
    }

    // ---- the scheduler's encoding of each boundary case on the hero's round trip --------------------
    {
        struct Case
        {
            const char* name;
            int64_t count;
            std::optional<int64_t> index;
            const char* check;
            const char* outcome;
            FieldStatus resolved_status;
            const char* resolved_reason;
            bool index_observed;
        };
        const Case s_Cases[] = {
            {"zero count, negative index", 0, -1, "negative", "hero_not_representable", FieldStatus::Unavailable, "not a sub-entity of that factory", true},
            {"zero count, index 0", 0, 0, "out_of_range", "unsupported", FieldStatus::Inconsistent, "index outside the sub-entity count", true},
            {"negative count", -1, std::nullopt, "count_invalid", "unsupported", FieldStatus::Inconsistent, "sub-entity count invalid", false},
            {"count beyond the plausibility bound", PlayerObservation::k_MaxSubEntityCount + 1, std::nullopt, "count_invalid", "unsupported", FieldStatus::Inconsistent, "sub-entity count invalid", false},
            {"negative index", 2, -1, "negative", "hero_not_representable", FieldStatus::Unavailable, "not a sub-entity of that factory", true},
            {"index equal to count", 2, 2, "out_of_range", "unsupported", FieldStatus::Inconsistent, "index outside the sub-entity count", true},
            {"index greater than count", 2, 7, "out_of_range", "unsupported", FieldStatus::Inconsistent, "index outside the sub-entity count", true},
            {"valid final index", 2, 1, "in_range", "same_entity", FieldStatus::Observed, "", true},
            {"valid first index", 2, 0, "in_range", "same_entity", FieldStatus::Observed, "", true},
        };

        for (const Case& s_Case : s_Cases)
        {
            const Result s_Result = RunEntity(Frame(100, 5000, true), [&](const EntityQuery&) { return EntityWithRoundTrip(ScriptedLookup(s_Case.count, s_Case.index)); }, EntityCommand(CommandTest::Uuid(1)));
            CHECK(s_Result.status == Status::Applied); // an inspection is applied whatever it found
            CHECK(IntField(s_Result, "rt_subentity_count") == s_Case.count); // the count as read, valid or not
            CHECK(TextField(s_Result, "rt_index_check") == s_Case.check);
            CHECK(TextField(s_Result, "rt_outcome") == s_Case.outcome);
            CHECK(FieldIs(s_Result, "rt_resolved", s_Case.resolved_status) && FieldIs(s_Result, "rt_same_entity", s_Case.resolved_status));
            CHECK(Reason(s_Result, "rt_resolved") == s_Case.resolved_reason);
            if (s_Case.index_observed)
                CHECK(IntField(s_Result, "rt_index") == *s_Case.index);
            else
                CHECK(FieldIs(s_Result, "rt_index", FieldStatus::Unavailable) && Reason(s_Result, "rt_index") == "count invalid; index not looked up");
            CHECK(s_Result.observed.size() == 34 && EncodeResult(s_Result).size() <= k_MaxLineBytes);
            if (s_Result.observed.size() != 34 || TextField(s_Result, "rt_index_check") != s_Case.check || TextField(s_Result, "rt_outcome") != s_Case.outcome)
                std::printf("  boundary case '%s': %zu entries, check %s, outcome %s\n", s_Case.name, s_Result.observed.size(), TextField(s_Result, "rt_index_check").c_str(), TextField(s_Result, "rt_outcome").c_str());
        }

        // a negative index is never conflated with an out-of-bounds one: the two cases differ in the
        // check, the outcome and the status, with the index value reported in both
        const Result s_Negative = RunEntity(Frame(100, 5000, true), [](const EntityQuery&) { return EntityWithRoundTrip(ScriptedLookup(2, -1)); }, EntityCommand(CommandTest::Uuid(1)));
        const Result s_Beyond = RunEntity(Frame(100, 5000, true), [](const EntityQuery&) { return EntityWithRoundTrip(ScriptedLookup(2, 2)); }, EntityCommand(CommandTest::Uuid(2)));
        CHECK(TextField(s_Negative, "rt_index_check") != TextField(s_Beyond, "rt_index_check"));
        CHECK(TextField(s_Negative, "rt_outcome") != TextField(s_Beyond, "rt_outcome"));
        CHECK(FieldIs(s_Negative, "rt_resolved", FieldStatus::Unavailable) && FieldIs(s_Beyond, "rt_resolved", FieldStatus::Inconsistent));
        CHECK(IntField(s_Negative, "rt_index") == -1 && IntField(s_Beyond, "rt_index") == 2);
    }

    // ---- the same boundaries on the candidate's lookup; the candidate echo and presence unchanged ----
    {
        const Result s_Beyond = RunEntity(Frame(100, 5000, true), [](const EntityQuery&) { return WithCandidateLookup(EntityWithRoundTrip(ScriptedLookup(2, 1)), ScriptedLookup(3, 3, false)); }, EntityCommand(CommandTest::Uuid(1)));
        CHECK(BoolField(s_Beyond, "cand_brick_present", true) && TextField(s_Beyond, "cand_brick") == k_HitmenBrick && TextField(s_Beyond, "cand_entity_id") == k_Agent48);
        CHECK(IntField(s_Beyond, "cand_subentity_count") == 3 && IntField(s_Beyond, "cand_index") == 3 && TextField(s_Beyond, "cand_index_check") == "out_of_range");
        CHECK(FieldIs(s_Beyond, "cand_resolved", FieldStatus::Inconsistent) && FieldIs(s_Beyond, "cand_distinct_from_local", FieldStatus::Inconsistent));
        CHECK(FieldIs(s_Beyond, "cand_character_id", FieldStatus::Unavailable) && FieldIs(s_Beyond, "cand_x", FieldStatus::Unavailable) && FieldIs(s_Beyond, "cand_hitman5", FieldStatus::Unavailable));
        CHECK(TextField(s_Beyond, "rt_outcome") == "same_entity"); // the hero's own round trip is unaffected
        CHECK(EncodeResult(s_Beyond).size() <= k_MaxLineBytes);

        const Result s_Invalid = RunEntity(Frame(100, 5000, true), [](const EntityQuery&) { return WithCandidateLookup(EntityWithRoundTrip(ScriptedLookup(2, 1)), ScriptedLookup(-3, std::nullopt)); }, EntityCommand(CommandTest::Uuid(2)));
        CHECK(IntField(s_Invalid, "cand_subentity_count") == -3 && FieldIs(s_Invalid, "cand_index", FieldStatus::Unavailable) && TextField(s_Invalid, "cand_index_check") == "count_invalid");
        CHECK(FieldIs(s_Invalid, "cand_resolved", FieldStatus::Inconsistent) && Reason(s_Invalid, "cand_resolved") == "sub-entity count invalid");

        const Result s_Negative = RunEntity(Frame(100, 5000, true), [](const EntityQuery&) { return WithCandidateLookup(EntityWithRoundTrip(ScriptedLookup(2, 1)), ScriptedLookup(3, -1)); }, EntityCommand(CommandTest::Uuid(3)));
        CHECK(IntField(s_Negative, "cand_index") == -1 && TextField(s_Negative, "cand_index_check") == "negative" && FieldIs(s_Negative, "cand_resolved", FieldStatus::Unavailable));

        const Result s_Valid = RunEntity(Frame(100, 5000, true), [](const EntityQuery&) { return WithCandidateLookup(EntityWithRoundTrip(ScriptedLookup(2, 1)), ScriptedLookup(3, 2, false)); }, EntityCommand(CommandTest::Uuid(4)));
        CHECK(IntField(s_Valid, "cand_index") == 2 && TextField(s_Valid, "cand_index_check") == "in_range" && BoolField(s_Valid, "cand_resolved", true) && BoolField(s_Valid, "cand_distinct_from_local", true));
        CHECK(s_Valid.observed.size() == 48 && s_Valid.observed.size() <= k_MaxObservedEntries && EncodeResult(s_Valid).size() <= k_MaxLineBytes);
    }

    // ---- the no-inspector and faulted paths carry the check marked, never defaulted -----------------
    {
        const Result s_None = RunEntity(Frame(100, 5000, true), nullptr, EntityCommand(CommandTest::Uuid(1)));
        CHECK(FieldIs(s_None, "rt_index_check", FieldStatus::Unsupported) && TextField(s_None, "rt_outcome") == "unsupported");
        const Result s_Fault = RunEntity(Frame(100, 5000, true), [](const EntityQuery& p_Query) { return PlayerObservation::AllEntity(FieldStatus::Unavailable, "fault during the entity reads", p_Query); }, EntityCommand(CommandTest::Uuid(2)));
        CHECK(FieldIs(s_Fault, "rt_index_check", FieldStatus::Unavailable) && Reason(s_Fault, "rt_index_check") == "fault during the entity reads" && TextField(s_Fault, "rt_outcome") == "unsupported");
    }
}
