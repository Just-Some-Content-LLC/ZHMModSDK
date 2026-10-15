// M6-B1 hardening tests: the three read-only inspection boundary corrections, each exercised on the
// pure rule the DLL applies and on the scheduler's encoding of a FAKE read set that reports the
// corresponding engine answer. Correction A: the blueprint-factory index bounds (a valid count
// before use, an index inside [0, count) before the one sub-entity access, a negative index kept
// distinct from a non-negative out-of-bounds one, every refusal a typed observation). Correction B:
// the final wire-length invariant (every reduction decided and re-checked on the actual encoded
// line, escapes and the encoder's truncations included, after every marker and note; near-boundary
// results, maximal texts, maximal reasons, JSON escaping, both stages, the entry cap, the reason
// bound, the maximal header, metadata preserved, the scheduler's own re-check; the final encoded
// byte length asserted in every case). Correction C: partial brick-scan semantics (a candidate's
// absence established only by a complete scan; an incomplete scan without a match unavailable with
// the scan bound as the reason; a candidate inside the scanned prefix still positively observable;
// display truncation distinct from lookup-scan truncation; counts below, at and above the bound).
// Fake values only: no engine pointer is constructed, valid or invalid; no memory is written; no
// setter exists on this path.
#include "TestHarness.h"

#include <algorithm>
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
        s_Reads.bricks.scanned = Field<int64_t>::Observed(static_cast<int64_t>(std::min(p_Bricks, PlayerObservation::k_MaxBricksScanned)));
        s_Reads.hero = HeroIdentity();
        s_Reads.round_trip = p_Trip;
        s_Reads.round_trip_outcome = PlayerObservation::ClassifyRoundTrip(s_Reads.round_trip);

        // the candidate absent from the scanned prefix, decided exactly as the reader decides it
        PlayerObservation::CandidateReads s_Candidate;
        s_Candidate.brick_id = k_HitmenBrick;
        s_Candidate.entity_id = k_Agent48;
        s_Candidate.brick_present = PlayerObservation::CandidatePresence(false, s_Reads.bricks);
        const std::string s_Why = s_Candidate.brick_present.observed() ? "candidate brick not loaded" : s_Candidate.brick_present.reason;
        PlayerObservation::MarkLookup(s_Candidate.lookup, FieldStatus::Unavailable, s_Why.c_str());
        PlayerObservation::MarkIdentity(s_Candidate.identity, FieldStatus::Unavailable, s_Why.c_str());
        s_Candidate.hitman5_resolved = Field<bool>::Of(FieldStatus::Unavailable, s_Why);
        s_Reads.candidate = s_Candidate;
        return s_Reads;
    }

    // The same read set with the candidate brick found inside the scanned prefix and its lookup scripted.
    EntityReads WithCandidateLookup(EntityReads p_Reads, const LookupFacts& p_Lookup)
    {
        auto& s_Candidate = *p_Reads.candidate;
        s_Candidate.brick_present = PlayerObservation::CandidatePresence(true, p_Reads.bricks);
        s_Candidate.lookup = p_Lookup;
        if (p_Lookup.resolved.observed() && *p_Lookup.resolved.value)
        {
            s_Candidate.identity = HeroIdentity(k_Agent48, k_HitmenBrick);
            s_Candidate.hitman5_resolved = Field<bool>::Observed(true);
        }
        else
        {
            PlayerObservation::MarkIdentity(s_Candidate.identity, FieldStatus::Unavailable, "candidate entity not resolved");
            s_Candidate.hitman5_resolved = Field<bool>::Of(FieldStatus::Unavailable, "candidate entity not resolved");
        }
        return p_Reads;
    }

    size_t CountPrefixed(const Result& p_Result, const char* p_Prefix)
    {
        size_t s_Count = 0;
        for (const auto& s_Entry : p_Result.observed)
            if (s_Entry.first.rfind(p_Prefix, 0) == 0)
                ++s_Count;
        return s_Count;
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

    // ---- correction B helpers ------------------------------------------------------------------------

    // A result header with every text at the encoder's bound (the native_at and mission generation
    // at 64 bytes, the largest link generation and frame), a version-2 line: the header the line
    // bound has to leave room for.
    Result MaximalHeader(const std::string& p_Reason = "")
    {
        Result s_Result;
        s_Result.protocol_version = k_ProtocolVersion2;
        s_Result.command_id = CommandTest::Uuid(1);
        s_Result.link_generation = UINT64_MAX;
        s_Result.adapter_instance_id = CommandTest::Uuid(99);
        s_Result.status = Status::Applied;
        s_Result.reason = p_Reason;
        s_Result.native_at = std::string(64, 'n');
        s_Result.frame = UINT64_MAX;
        s_Result.mission_generation = std::string(64, 'g');
        return s_Result;
    }

    size_t Encoded(const Result& p_Result)
    {
        return EncodeResult(p_Result).size();
    }

    size_t MarkerCount(const Result& p_Result)
    {
        size_t s_Count = 0;
        for (const auto& s_Entry : p_Result.observed)
            if (s_Entry.first == "reasons_dropped" || s_Entry.first == "observed_reduced")
                ++s_Count;
        return s_Count;
    }

    bool MarkersLast(const Result& p_Result)
    {
        const size_t s_Count = MarkerCount(p_Result);
        if (s_Count == 0 || p_Result.observed.size() < s_Count)
            return false;
        for (size_t i = p_Result.observed.size() - s_Count; i < p_Result.observed.size(); ++i)
            if (p_Result.observed[i].first != "reasons_dropped" && p_Result.observed[i].first != "observed_reduced")
                return false;
        return p_Result.observed.back().first == (s_Count == 2 ? "observed_reduced" : "reasons_dropped");
    }

    // The metadata ReduceToFit must never touch.
    bool SameMetadata(const Result& p_Before, const Result& p_After)
    {
        return p_Before.protocol_version == p_After.protocol_version && p_Before.command_id == p_After.command_id && p_Before.link_generation == p_After.link_generation
            && p_Before.adapter_instance_id == p_After.adapter_instance_id && p_Before.status == p_After.status && p_Before.native_at == p_After.native_at
            && p_Before.frame == p_After.frame && p_Before.mission_generation == p_After.mission_generation;
    }

    // The raw (unencoded) byte count of the observed texts and keys: what a naive measure would add up.
    size_t RawTextBytes(const Result& p_Result)
    {
        size_t s_Bytes = 0;
        for (const auto& s_Entry : p_Result.observed)
            s_Bytes += s_Entry.first.size() + s_Entry.second.text.size() + s_Entry.second.field_reason.size();
        return s_Bytes;
    }

    void AppendText(Result& p_Result, const std::string& p_Name, const std::string& p_Text)
    {
        OutfitObservation::Append(p_Result.observed, p_Name.c_str(), Field<std::string>::Observed(p_Text));
    }

    void AppendUnavailable(Result& p_Result, const std::string& p_Name, const std::string& p_Reason)
    {
        OutfitObservation::Append(p_Result.observed, p_Name.c_str(), Field<std::string>::Of(FieldStatus::Unavailable, p_Reason));
    }

    // A result whose encoded line is exactly p_Target bytes: the per-field cost of a 64-byte text
    // field with a two-digit name is measured once; enough of them are added to come within the
    // text bound of the target; the last field's text is sized to land on it exactly (asserted).
    Result ExactlySized(size_t p_Target)
    {
        Result s_Result = MaximalHeader();
        Result s_Probe = s_Result;
        AppendText(s_Probe, "last", "");
        const size_t s_Base = Encoded(s_Probe);
        AppendText(s_Probe, "f_10", std::string(64, 'x'));
        const size_t s_Delta = Encoded(s_Probe) - s_Base;
        CHECK(s_Delta > 64 && s_Delta < k_MaxObservedTextBytes);
        const size_t s_Fields = p_Target > s_Base + k_MaxObservedTextBytes ? (p_Target - s_Base - k_MaxObservedTextBytes) / s_Delta + 1 : 0;
        CHECK(s_Fields < 90);
        for (size_t i = 0; i < s_Fields; ++i)
            AppendText(s_Result, "f_" + std::to_string(10 + i), std::string(64, 'x'));
        AppendText(s_Result, "last", "");
        const size_t s_Empty = Encoded(s_Result);
        CHECK(s_Empty <= p_Target && p_Target - s_Empty <= k_MaxObservedTextBytes);
        s_Result.observed.back().second.text = std::string(p_Target - s_Empty, 'y');
        CHECK(Encoded(s_Result) == p_Target);
        return s_Result;
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
            CHECK(s_Result.observed.size() == 35 && EncodeResult(s_Result).size() <= k_MaxLineBytes);
            if (s_Result.observed.size() != 35 || TextField(s_Result, "rt_index_check") != s_Case.check || TextField(s_Result, "rt_outcome") != s_Case.outcome)
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

    // ==== Correction B: the final wire-length invariant =============================================

    // ---- near the boundary: exactly at the bound nothing happens; one byte over is reduced ---------
    {
        Result s_At = ExactlySized(k_MaxLineBytes);
        CHECK(Encoded(s_At) == k_MaxLineBytes);
        const Result s_AtBefore = s_At;
        CHECK(!CommandScheduler::ReduceToFit(s_At));
        CHECK(Encoded(s_At) == k_MaxLineBytes && s_At.reason.empty() && MarkerCount(s_At) == 0 && s_At.observed.size() == s_AtBefore.observed.size());
        CHECK(CommandScheduler::WithinLineBound(s_At));

        Result s_Under = ExactlySized(k_MaxLineBytes - 1);
        CHECK(Encoded(s_Under) == k_MaxLineBytes - 1 && !CommandScheduler::ReduceToFit(s_Under) && Encoded(s_Under) == k_MaxLineBytes - 1);

        Result s_Over = ExactlySized(k_MaxLineBytes + 1);
        CHECK(Encoded(s_Over) == k_MaxLineBytes + 1);
        const Result s_OverBefore = s_Over;
        CHECK(CommandScheduler::ReduceToFit(s_Over));
        const size_t s_Final = Encoded(s_Over);
        CHECK(s_Final <= k_MaxLineBytes);
        // no field reasons existed, so the first stage alone could not have brought the line back:
        // entries were dropped, both markers are last, both notes are in the reason
        CHECK(MarkerCount(s_Over) == 2 && MarkersLast(s_Over));
        CHECK(s_Over.reason.find("field reasons dropped") != std::string::npos && s_Over.reason.find("inspection reduced") != std::string::npos);
        CHECK(s_Over.observed.size() < s_OverBefore.observed.size() + 2);
        CHECK(SameMetadata(s_OverBefore, s_Over));
        CHECK(CommandScheduler::WithinLineBound(s_Over));
        std::printf("line bound: one byte over the bound (%zu) reduced to %zu bytes, %zu of %zu plain entries kept\n", k_MaxLineBytes + 1, s_Final, s_Over.observed.size() - 2, s_OverBefore.observed.size());

        // the reduction is the measured one: dropping one fewer entry would not have fit
        Result s_OneMore = s_Over;
        const size_t s_KeptPlain = s_Over.observed.size() - 2;
        s_OneMore.observed.insert(s_OneMore.observed.begin() + static_cast<std::ptrdiff_t>(s_KeptPlain), s_OverBefore.observed[s_KeptPlain]);
        CHECK(Encoded(s_OneMore) > k_MaxLineBytes);

        // already reduced: a second call finds it within the bound and changes nothing
        const Result s_Again = s_Over;
        CHECK(!CommandScheduler::ReduceToFit(s_Over) && Encoded(s_Over) == s_Final && s_Over.reason == s_Again.reason && s_Over.observed.size() == s_Again.observed.size());
    }

    // ---- maximal field texts: 48 entries at the text bound, dropped from the end until it fits ---
    {
        Result s_Result = MaximalHeader();
        for (int i = 0; i < 48; ++i)
            AppendText(s_Result, "field_" + std::to_string(i), std::string(k_MaxObservedTextBytes, 'x'));
        const Result s_Before = s_Result;
        const size_t s_Initial = Encoded(s_Result);
        CHECK(s_Initial > k_MaxLineBytes);
        CHECK(CommandScheduler::ReduceToFit(s_Result));
        const size_t s_Final = Encoded(s_Result);
        CHECK(s_Final <= k_MaxLineBytes);
        CHECK(MarkerCount(s_Result) == 2 && MarkersLast(s_Result) && s_Result.observed.size() <= k_MaxObservedEntries);
        // the kept entries are the first ones, intact (value, status, text at its full length)
        for (size_t i = 0; i + 2 < s_Result.observed.size(); ++i)
            CHECK(s_Result.observed[i].first == "field_" + std::to_string(i) && s_Result.observed[i].second.text.size() == k_MaxObservedTextBytes && s_Result.observed[i].second.field_status && *s_Result.observed[i].second.field_status == FieldStatus::Observed);
        CHECK(SameMetadata(s_Before, s_Result));
        std::printf("line bound: 48 maximal texts %zu bytes reduced to %zu bytes, %zu plain entries kept\n", s_Initial, s_Final, s_Result.observed.size() - 2);
    }

    // ---- maximal reasons: the first stage keeps every field and status; the entry cap rule --------
    {
        // 46 entries (the players inspection's count) unavailable with 40-byte reasons: over the
        // bound with the reasons, within it without them; only the reasons go
        Result s_Result = MaximalHeader("engine reads unsupported on this frame source; frame values only");
        for (int i = 0; i < 46; ++i)
            AppendUnavailable(s_Result, "field_" + std::to_string(i), std::string(k_MaxObservedReasonBytes, 'r'));
        const Result s_Before = s_Result;
        const size_t s_Initial = Encoded(s_Result);
        CHECK(s_Initial > k_MaxLineBytes);
        CHECK(CommandScheduler::ReduceToFit(s_Result));
        const size_t s_Final = Encoded(s_Result);
        CHECK(s_Final <= k_MaxLineBytes);
        CHECK(s_Result.observed.size() == 47 && MarkerCount(s_Result) == 1 && s_Result.observed.back().first == "reasons_dropped");
        for (size_t i = 0; i < 46; ++i)
            CHECK(s_Result.observed[i].first == "field_" + std::to_string(i) && s_Result.observed[i].second.field_status && *s_Result.observed[i].second.field_status == FieldStatus::Unavailable && s_Result.observed[i].second.field_reason.empty());
        CHECK(s_Result.reason == "engine reads unsupported on this frame source; frame values only; field reasons dropped to fit the result line bound");
        CHECK(SameMetadata(s_Before, s_Result));
        std::printf("line bound: 46 maximal reasons %zu bytes reduced to %zu bytes by dropping the reasons only\n", s_Initial, s_Final);

        // at the entry cap (48) the first-stage marker takes the place of the last entry, which
        // makes it an entry drop too: both markers, both notes, 46 plain entries kept with status
        Result s_Cap = MaximalHeader();
        for (int i = 0; i < 48; ++i)
            AppendUnavailable(s_Cap, "field_" + std::to_string(i), std::string(k_MaxObservedReasonBytes, 'r'));
        CHECK(Encoded(s_Cap) > k_MaxLineBytes);
        CHECK(CommandScheduler::ReduceToFit(s_Cap));
        CHECK(Encoded(s_Cap) <= k_MaxLineBytes);
        CHECK(s_Cap.observed.size() == 48 && MarkerCount(s_Cap) == 2 && MarkersLast(s_Cap));
        CHECK(s_Cap.observed[45].first == "field_45" && s_Cap.observed[45].second.field_reason.empty() && *s_Cap.observed[45].second.field_status == FieldStatus::Unavailable);
        CHECK(s_Cap.reason == "field reasons dropped to fit the result line bound; inspection reduced to fit the result line bound");
    }

    // ---- JSON escaping: measured in the encoded form, never the raw one --------------------------
    {
        // quotation marks double on the wire; control characters become six bytes each. The raw
        // text bytes sum well under the bound; the encoded line does not.
        Result s_Result = MaximalHeader();
        for (int i = 0; i < 10; ++i)
            AppendText(s_Result, "q_" + std::to_string(i), std::string(k_MaxObservedTextBytes, '"'));
        for (int i = 0; i < 4; ++i)
            AppendText(s_Result, "c_" + std::to_string(i), std::string(k_MaxObservedTextBytes, '\x01'));
        AppendText(s_Result, "b", std::string(k_MaxObservedTextBytes, '\\'));
        const Result s_Before = s_Result;
        const size_t s_Raw = RawTextBytes(s_Result);
        const size_t s_Initial = Encoded(s_Result);
        CHECK(s_Raw < k_MaxLineBytes && s_Initial > k_MaxLineBytes);
        CHECK(CommandScheduler::ReduceToFit(s_Result));
        const size_t s_Final = Encoded(s_Result);
        CHECK(s_Final <= k_MaxLineBytes && MarkerCount(s_Result) == 2 && MarkersLast(s_Result) && SameMetadata(s_Before, s_Result));
        // a text kept is kept whole: the encoder escapes it, the reduction never edits it
        CHECK(!s_Result.observed.empty() && s_Result.observed[0].second.text == std::string(k_MaxObservedTextBytes, '"'));
        CHECK(EncodeResult(s_Result).find(std::string(16, '\\')) != std::string::npos || EncodeResult(s_Result).find("\\\"\\\"") != std::string::npos);
        std::printf("line bound: escaped texts raw %zu bytes, encoded %zu bytes, reduced to %zu bytes with %zu plain entries kept\n", s_Raw, s_Initial, s_Final, s_Result.observed.size() - 2);

        // the same with reasons: an escaped reason is measured in its encoded form too
        Result s_Reasons = MaximalHeader();
        for (int i = 0; i < 46; ++i)
            AppendUnavailable(s_Reasons, "field_" + std::to_string(i), std::string(k_MaxObservedReasonBytes, '"'));
        CHECK(RawTextBytes(s_Reasons) < k_MaxLineBytes && Encoded(s_Reasons) > k_MaxLineBytes);
        CHECK(CommandScheduler::ReduceToFit(s_Reasons) && Encoded(s_Reasons) <= k_MaxLineBytes && s_Reasons.observed.size() == 47);
    }

    // ---- the reason bound: the notes survive a 200-byte reason; the existing text gives way --------
    {
        Result s_Result = MaximalHeader(std::string(k_MaxReasonBytes, 'r'));
        for (int i = 0; i < 48; ++i)
            AppendText(s_Result, "field_" + std::to_string(i), std::string(k_MaxObservedTextBytes, 'x'));
        CHECK(CommandScheduler::ReduceToFit(s_Result));
        CHECK(Encoded(s_Result) <= k_MaxLineBytes);
        CHECK(s_Result.reason.size() <= k_MaxReasonBytes);
        CHECK(s_Result.reason.find("field reasons dropped to fit the result line bound") != std::string::npos);
        CHECK(s_Result.reason.size() >= std::string("inspection reduced to fit the result line bound").size() && s_Result.reason.compare(s_Result.reason.size() - std::string("inspection reduced to fit the result line bound").size(), std::string::npos, "inspection reduced to fit the result line bound") == 0);
        CHECK(s_Result.reason.rfind("rrr", 0) == 0); // what remains of the original text comes first
        CHECK(s_Result.reason == std::string(99, 'r') + "; field reasons dropped to fit the result line bound; inspection reduced to fit the result line bound");
        // the encoded reason is the reason itself: nothing the encoder truncates away
        CHECK(EncodeResult(s_Result).find("\"reason\":\"" + s_Result.reason + "\"") != std::string::npos);

        // a multi-byte character at the cut is not split
        std::string s_Utf8;
        while (s_Utf8.size() < k_MaxReasonBytes)
            s_Utf8 += "\xC3\xA9"; // "é"
        Result s_Multi = MaximalHeader(s_Utf8);
        for (int i = 0; i < 48; ++i)
            AppendText(s_Multi, "field_" + std::to_string(i), std::string(k_MaxObservedTextBytes, 'x'));
        CHECK(CommandScheduler::ReduceToFit(s_Multi) && Encoded(s_Multi) <= k_MaxLineBytes && s_Multi.reason.size() <= k_MaxReasonBytes);
        const size_t s_Semicolon = s_Multi.reason.find("; ");
        CHECK(s_Semicolon != std::string::npos && s_Semicolon % 2 == 0 && s_Multi.reason.compare(0, s_Semicolon, s_Utf8, 0, s_Semicolon) == 0);
    }

    // ---- the maximal header alone (the last resort) always fits ------------------------------------
    {
        Result s_Header = MaximalHeader(std::string(k_MaxReasonBytes, '"'));
        s_Header.native_at = std::string(64, '"');
        s_Header.mission_generation = std::string(64, '"');
        s_Header.observed.emplace_back("reasons_dropped", ObservedValue{ObservedValue::Kind::Bool, true});
        s_Header.observed.emplace_back("observed_reduced", ObservedValue{ObservedValue::Kind::Bool, true});
        const size_t s_Size = Encoded(s_Header);
        CHECK(s_Size <= k_MaxLineBytes);
        CHECK(!CommandScheduler::ReduceToFit(s_Header));
        std::printf("line bound: the maximal escaped header with both markers encodes to %zu bytes (bound %zu)\n", s_Size, k_MaxLineBytes);
    }

    // ---- the realistic M6-B1 shapes: measured, and within the bound without any reduction ----------
    {
        Result s_Players = MaximalHeader();
        const std::string s_MaxReason(k_MaxObservedReasonBytes, 'r');
        PlayerObservation::AppendRegistry(s_Players.observed, PlayerObservation::AllRegistry(FieldStatus::Unavailable, s_MaxReason.c_str()));
        OutfitObservation::Append(s_Players.observed, "reads_on_frame", Field<bool>::Observed(true));
        const size_t s_PlayersInitial = Encoded(s_Players);
        const bool s_PlayersReduced = CommandScheduler::ReduceToFit(s_Players);
        CHECK(Encoded(s_Players) <= k_MaxLineBytes);
        CHECK(!s_PlayersReduced || (MarkerCount(s_Players) == 1 && s_Players.observed.size() == 44));

        Result s_Entity = MaximalHeader();
        EntityReads s_Long = WithCandidateLookup(EntityWithRoundTrip(ScriptedLookup(2, 1)), ScriptedLookup(3, 2, false));
        s_Long.hero.owning_brick = Field<std::string>::Observed(std::string(k_MaxObservedTextBytes, 'f'));
        PlayerObservation::AppendEntity(s_Entity.observed, s_Long);
        OutfitObservation::Append(s_Entity.observed, "reads_on_frame", Field<bool>::Observed(true));
        const size_t s_EntitySize = Encoded(s_Entity);
        CHECK(s_EntitySize <= k_MaxLineBytes && !CommandScheduler::ReduceToFit(s_Entity));
        std::printf("line bound: all-unavailable registry with maximal reasons %zu bytes (%s), longest candidate-present entity read-back %zu bytes (no reduction), bound %zu\n", s_PlayersInitial, s_PlayersReduced ? "reasons dropped" : "no reduction", s_EntitySize, k_MaxLineBytes);
    }

    // ---- the scheduler re-checks the invariant on what it emits -------------------------------------
    {
        // a FAKE entity read set with every text at the bound and every other field unavailable with
        // a maximal reason: the emitted result is within the bound, marked, and the re-check never
        // fired (unbounded stays 0)
        CommandScheduler::Stats s_Stats;
        const Result s_Result = RunEntity(Frame(100, 5000, true), [](const EntityQuery& p_Query) {
            const std::string s_MaxReason(k_MaxObservedReasonBytes, 'r');
            EntityReads s_Reads = PlayerObservation::AllEntity(FieldStatus::Unavailable, s_MaxReason.c_str(), p_Query);
            s_Reads.thread_id = GetCurrentThreadId();
            s_Reads.bricks.count = Field<int64_t>::Observed(21);
            for (int i = 0; i < 21; ++i)
                s_Reads.bricks.ids.push_back(std::string(k_MaxObservedTextBytes, 'a' + static_cast<char>(i % 26)));
            s_Reads.hero.entity_id = Field<std::string>::Observed(std::string(k_MaxObservedTextBytes, 'e'));
            s_Reads.hero.owning_brick = Field<std::string>::Observed(std::string(k_MaxObservedTextBytes, 'o'));
            s_Reads.hero.character_id = Field<std::string>::Observed(std::string(k_MaxObservedTextBytes, 'c'));
            s_Reads.hero.initial_outfit_id = Field<std::string>::Observed(std::string(k_MaxObservedTextBytes, 'i'));
            s_Reads.candidate->brick_present = Field<bool>::Observed(true);
            s_Reads.candidate->identity.character_id = Field<std::string>::Observed(std::string(k_MaxObservedTextBytes, 'k'));
            return s_Reads;
        }, EntityCommand(CommandTest::Uuid(1), std::string(k_HitmenBrick), std::string(k_Agent48)), &s_Stats);
        CHECK(s_Result.status == Status::Applied);
        CHECK(Encoded(s_Result) <= k_MaxLineBytes && CommandScheduler::WithinLineBound(s_Result));
        CHECK(s_Stats.unbounded == 0 && s_Stats.entities_inspected == 1);
        CHECK(s_Stats.reduced == (MarkerCount(s_Result) > 0 ? 1u : 0u));
        CHECK(MarkerCount(s_Result) == 0 || MarkersLast(s_Result));
        CHECK(s_Result.command_id == CommandTest::Uuid(1) && s_Result.frame && *s_Result.frame == 100 && s_Result.mission_generation && *s_Result.mission_generation == k_Gen1 && s_Result.link_generation == 1);
        std::printf("line bound: the scheduler's maximal FAKE entity result encodes to %zu bytes (%s; reduced %llu, unbounded %llu)\n", Encoded(s_Result), MarkerCount(s_Result) ? "reduced" : "not reduced", s_Stats.reduced, s_Stats.unbounded);
    }

    // ==== Correction C: partial brick-scan semantics ===============================================

    // ---- the pure rule: presence from a bounded prefix scan ------------------------------------------
    {
        const auto s_Bricks = [](int64_t p_Count) {
            PlayerObservation::BrickReads s_Reads;
            s_Reads.count = Field<int64_t>::Observed(p_Count);
            s_Reads.scanned = Field<int64_t>::Observed(std::min<int64_t>(p_Count, static_cast<int64_t>(PlayerObservation::k_MaxBricksScanned)));
            s_Reads.truncated = Field<bool>::Observed(p_Count > static_cast<int64_t>(PlayerObservation::k_MaxBrickIds));
            return s_Reads;
        };
        CHECK(PlayerObservation::k_MaxBricksScanned == 64 && PlayerObservation::k_MaxBrickIds == 21);

        // the scan is complete up to and including the bound, partial beyond it
        CHECK(PlayerObservation::ScanComplete(s_Bricks(0)) && PlayerObservation::ScanComplete(s_Bricks(23)) && PlayerObservation::ScanComplete(s_Bricks(63)) && PlayerObservation::ScanComplete(s_Bricks(64)));
        CHECK(!PlayerObservation::ScanComplete(s_Bricks(65)) && !PlayerObservation::ScanComplete(s_Bricks(70)) && !PlayerObservation::ScanComplete(s_Bricks(100000)));

        // found inside the scanned prefix: present, whatever the count
        for (const int64_t s_Count : {1, 23, 63, 64, 65, 70})
        {
            const auto s_Presence = PlayerObservation::CandidatePresence(true, s_Bricks(s_Count));
            CHECK(s_Presence.observed() && *s_Presence.value);
        }
        // absent: "false" only from a complete scan
        for (const int64_t s_Count : {0, 1, 23, 63, 64})
        {
            const auto s_Presence = PlayerObservation::CandidatePresence(false, s_Bricks(s_Count));
            CHECK(s_Presence.observed() && !*s_Presence.value && s_Presence.reason.empty());
        }
        // absent from a partial scan: unavailable with the scan bound as the reason, never "false"
        for (const int64_t s_Count : {65, 70, 1000})
        {
            const auto s_Presence = PlayerObservation::CandidatePresence(false, s_Bricks(s_Count));
            CHECK(!s_Presence.observed() && s_Presence.status == FieldStatus::Unavailable && s_Presence.reason == "scan bound: 64/" + std::to_string(s_Count) + " bricks scanned");
            CHECK(s_Presence.reason.size() <= k_MaxObservedReasonBytes);
        }
        // an unavailable brick list: unavailable, never "false"
        PlayerObservation::BrickReads s_None;
        s_None.count = Field<int64_t>::Of(FieldStatus::Unavailable, "scene context null");
        s_None.scanned = Field<int64_t>::Of(FieldStatus::Unavailable, "scene context null");
        s_None.truncated = Field<bool>::Of(FieldStatus::Unavailable, "scene context null");
        CHECK(!PlayerObservation::ScanComplete(s_None));
        const auto s_NonePresence = PlayerObservation::CandidatePresence(false, s_None);
        CHECK(s_NonePresence.status == FieldStatus::Unavailable && s_NonePresence.reason == "brick list unavailable");

        // display truncation and lookup-scan truncation are distinct observations
        CHECK(*s_Bricks(23).truncated.value && PlayerObservation::ScanComplete(s_Bricks(23)));   // display partial, scan complete
        CHECK(*s_Bricks(70).truncated.value && !PlayerObservation::ScanComplete(s_Bricks(70)));  // both partial
        CHECK(!*s_Bricks(10).truncated.value && PlayerObservation::ScanComplete(s_Bricks(10)));  // neither
        CHECK(*s_Bricks(64).truncated.value && PlayerObservation::ScanComplete(s_Bricks(64)));   // the bound itself is complete

        // the owner that matched no scanned brick root: "neither" only from a complete scan
        CHECK(std::string(PlayerObservation::UnmatchedOwnerReason(s_Bricks(23))) == "owner is neither a loaded brick root nor the scene root");
        CHECK(std::string(PlayerObservation::UnmatchedOwnerReason(s_Bricks(70))) == "owner not in the scanned bricks (bound)");
        CHECK(std::string(PlayerObservation::UnmatchedOwnerReason(s_Bricks(70))).size() <= k_MaxObservedReasonBytes);
    }

    // ---- the scheduler's encoding: counts below, at and above the bound, candidate absent or found ----
    {
        struct Case
        {
            size_t count;
            bool found;
            const char* expect; // "false" | "unavailable" | "true"
            size_t entries;
        };
        const Case s_Cases[] = {
            {0, false, "false", 32},        // no id field; a complete (empty) scan
            {5, false, "false", 33},        // one id field
            {23, false, "false", 35},       // display truncated (21 of 23 ids), scan complete
            {63, false, "false", 35},       // one below the bound
            {64, false, "false", 35},       // at the bound: still complete
            {65, false, "unavailable", 35}, // one above: absence not established
            {70, false, "unavailable", 35},
            {23, true, "true", 48},         // found, complete scan
            {65, true, "true", 48},         // found inside the prefix of a partial scan
            {70, true, "true", 48},
        };

        for (const Case& s_Case : s_Cases)
        {
            const Result s_Result = RunEntity(Frame(100, 5000, true), [&](const EntityQuery&) {
                EntityReads s_Reads = EntityWithRoundTrip(ScriptedLookup(2, 1), s_Case.count);
                return s_Case.found ? WithCandidateLookup(s_Reads, ScriptedLookup(3, 2, false)) : s_Reads;
            }, EntityCommand(CommandTest::Uuid(1)));

            const int64_t s_Count = static_cast<int64_t>(s_Case.count);
            const int64_t s_Scanned = std::min<int64_t>(s_Count, 64);
            const size_t s_IdFields = std::min<size_t>((std::min<size_t>(s_Case.count, 21) + 6) / 7, 3);
            CHECK(s_Result.status == Status::Applied);
            CHECK(IntField(s_Result, "brick_count") == s_Count && IntField(s_Result, "bricks_scanned") == s_Scanned);
            CHECK(BoolField(s_Result, "bricks_truncated", s_Case.count > 21) && CountPrefixed(s_Result, "brick_ids_") == s_IdFields);
            CHECK(!Observed(s_Result, "cand_spatial"));
            CHECK(TextField(s_Result, "cand_brick") == k_HitmenBrick && TextField(s_Result, "cand_entity_id") == k_Agent48);

            const std::string s_Expect = s_Case.expect;
            if (s_Expect == "false")
            {
                CHECK(BoolField(s_Result, "cand_brick_present", false) && Reason(s_Result, "cand_brick_present").empty());
                CHECK(!Observed(s_Result, "cand_index") && !Observed(s_Result, "cand_hitman5"));
            }
            else if (s_Expect == "unavailable")
            {
                CHECK(FieldIs(s_Result, "cand_brick_present", FieldStatus::Unavailable));
                CHECK(Reason(s_Result, "cand_brick_present") == "scan bound: 64/" + std::to_string(s_Count) + " bricks scanned");
                CHECK(!Observed(s_Result, "cand_index") && !Observed(s_Result, "cand_hitman5"));
            }
            else
            {
                CHECK(BoolField(s_Result, "cand_brick_present", true) && IntField(s_Result, "cand_index") == 2 && TextField(s_Result, "cand_index_check") == "in_range");
                CHECK(BoolField(s_Result, "cand_resolved", true) && BoolField(s_Result, "cand_hitman5", true) && BoolField(s_Result, "cand_distinct_from_local", true));
            }
            CHECK(s_Result.observed.size() == s_Case.entries && s_Result.observed.size() <= k_MaxObservedEntries && EncodeResult(s_Result).size() <= k_MaxLineBytes);
            if (s_Result.observed.size() != s_Case.entries)
                std::printf("  brick-scan case count %zu found %d: %zu entries (expected %zu)\n", s_Case.count, s_Case.found ? 1 : 0, s_Result.observed.size(), s_Case.entries);
        }

        // the hero's owning brick beyond a partial scan is reported as such, never as "neither"
        const Result s_Owner = RunEntity(Frame(100, 5000, true), [](const EntityQuery&) {
            EntityReads s_Reads = EntityWithRoundTrip(ScriptedLookup(2, 1), 70);
            s_Reads.hero.owning_brick = Field<std::string>::Of(FieldStatus::Unavailable, PlayerObservation::UnmatchedOwnerReason(s_Reads.bricks));
            return s_Reads;
        }, EntityCommand(CommandTest::Uuid(2)));
        CHECK(FieldIs(s_Owner, "hero_owning_brick", FieldStatus::Unavailable) && Reason(s_Owner, "hero_owning_brick") == "owner not in the scanned bricks (bound)");
        CHECK(TextField(s_Owner, "rt_outcome") == "same_entity"); // the lookup itself goes through the owning entity, not the scan

        // the no-inspector and faulted paths carry the scan count marked, never defaulted
        const Result s_None = RunEntity(Frame(100, 5000, true), nullptr, EntityCommand(CommandTest::Uuid(3)));
        CHECK(FieldIs(s_None, "bricks_scanned", FieldStatus::Unsupported) && FieldIs(s_None, "cand_brick_present", FieldStatus::Unsupported));
    }
}
