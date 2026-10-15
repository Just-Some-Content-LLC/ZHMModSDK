// M6-B1 tests: the pure rules of PlayerObservation (hex ids, the registry-word classification, the
// brick-id grouping, the round-trip classification), the deterministic encoding of both read sets
// and its bounds, the scheduling of relay.inspect.players v1 and relay.inspect.entity v1 with fake
// inspectors (registry unavailable, layout unsupported, slot 0 occupied, slots 1–3 empty, an extra
// occupied slot, a missing hero interface, empty and oversized brick lists, the candidate absent
// and present, a valid round trip, a round trip returning a different entity, a hero without an
// owning entity, a factory type mismatch, a missing sub-entity, a null entity reference, a dirty
// transform, invalid entity flags, a mission restart with a generation change, menu-time hero
// resolution, result reduction), and the absence of any engine setter. Fake values only: no engine
// pointer is constructed, valid or invalid; no memory is written; no setter exists on this path.
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
using PlayerObservation::LookupFacts;
using PlayerObservation::RegistryReads;
using PlayerObservation::RoundTrip;
using PlayerObservation::SlotReads;

namespace
{
    constexpr const char* k_Suit = "874c4c48-0a8b-49e9-883e-49fc5f1fb051";
    constexpr const char* k_Zeros = "00000000-0000-0000-0000-000000000000";
    constexpr const char* k_Gen1 = "2516103582917844185-a105ec5f-3a75-46a3-8ecd-e72fd15b176c";
    constexpr const char* k_Gen2 = "2516103582027577704-30ccebb9-8138-4c72-9eb9-1a289b4fba33";
    constexpr const char* k_HitmenBrick = "007c57a6ea581f8c";   // a FAKE stand-in for the historical brick id (not the engine's hash)
    constexpr const char* k_Agent48 = "feede715906f747f";
    constexpr const char* k_HeroId = "feed9a0d70d546f4";        // a FAKE authored id for the local hero
    constexpr const char* k_HeroBrick = "0012e5a7b1c9d3ee";     // a FAKE loaded brick that owns the hero

    Command Players(const std::string& p_Id, uint64_t p_Generation = 1, uint32_t p_Ttl = k_MaxTtlMs)
    {
        Command s_Command;
        s_Command.command_id = p_Id;
        s_Command.link_generation = p_Generation;
        s_Command.ttl_ms = p_Ttl;
        s_Command.name = "relay.inspect.players";
        s_Command.version = 1;
        s_Command.entry = Entry::RelayInspectPlayers1;
        return s_Command;
    }

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

    CommandScheduler::FrameObservation Frame(uint64_t p_Frame, uint64_t p_NowMs, bool p_Playing, std::optional<std::string> p_Generation = k_Gen1, bool p_Available = true)
    {
        CommandScheduler::FrameObservation s_Frame;
        s_Frame.frame = p_Frame;
        s_Frame.now_mono_ms = p_NowMs;
        s_Frame.native_at = "t";
        s_Frame.thread_id = GetCurrentThreadId();
        s_Frame.scene_available = p_Available;
        s_Frame.scene.available = p_Available;
        s_Frame.scene.scene_resource = p_Playing ? "assembly:/_PRO/Scenes/Missions/Paris/_Scene_FashionShowHit_01.entity" : "assembly:/_PRO/Scenes/Frontend/MainMenu.entity";
        s_Frame.scene.scene_loaded = true;
        s_Frame.scene.loading_stage = 8;
        s_Frame.scene.scene_type = p_Playing ? "mission" : "";
        s_Frame.scene.codename_hint = p_Playing ? "Peacock" : "";
        s_Frame.mission_playing = p_Playing;
        if (p_Playing)
            s_Frame.mission_generation = p_Generation;
        s_Frame.link_generation = 1;
        s_Frame.adapter_instance_id = CommandTest::Uuid(99);
        s_Frame.relay_transition_epoch = 1;
        s_Frame.previous_frame_playing = p_Playing;
        return s_Frame;
    }

    SlotReads EmptySlot()
    {
        SlotReads s_Slot;
        s_Slot.player_id = Field<int64_t>::Observed(-1);
        s_Slot.local_flag = Field<bool>::Observed(true); // experiment 4: true on the empty slots too
        s_Slot.connected_flag = Field<bool>::Observed(false);
        s_Slot.net_player = Field<bool>::Observed(false);
        s_Slot.replica = Field<bool>::Observed(false);
        s_Slot.entity_present = Field<bool>::Observed(false);
        s_Slot.hero_resolved = Field<bool>::Of(FieldStatus::Unavailable, "entity reference null");
        s_Slot.flags_a0 = Field<int64_t>::Observed(-1);
        s_Slot.session_id = Field<std::string>::Of(FieldStatus::Unavailable, "session id empty");
        s_Slot.outfit_id = Field<std::string>::Observed(k_Zeros);
        s_Slot.character_id = Field<std::string>::Observed(k_Zeros);
        return s_Slot;
    }

    SlotReads LocalSlot(bool p_InMission, const char* p_Generation = k_Gen1)
    {
        SlotReads s_Slot = EmptySlot();
        s_Slot.player_id = Field<int64_t>::Observed(0);
        s_Slot.entity_present = Field<bool>::Observed(true);
        s_Slot.hero_resolved = Field<bool>::Observed(true);
        s_Slot.flags_a0 = Field<int64_t>::Observed(0);
        if (p_InMission)
        {
            s_Slot.session_id = Field<std::string>::Observed(p_Generation);
            s_Slot.outfit_id = Field<std::string>::Observed(k_Suit);
        }
        return s_Slot;
    }

    // A fully observed single-player registry, as experiment 4 saw it (F1, F7).
    RegistryReads SinglePlayer(bool p_InMission, const char* p_Generation = k_Gen1)
    {
        RegistryReads s_Reads;
        s_Reads.thread_id = GetCurrentThreadId();
        s_Reads.registry_available = Field<bool>::Observed(true);
        s_Reads.words = Field<std::string>::Observed("slot0,0,0x4000000000000101");
        s_Reads.slots[0] = LocalSlot(p_InMission, p_Generation);
        for (size_t i = 1; i < PlayerObservation::k_SlotCount; ++i)
            s_Reads.slots[i] = EmptySlot();
        s_Reads.hero_kit_id = Field<std::string>::Observed(k_Suit);
        s_Reads.slot0_matches_m5 = Field<bool>::Observed(p_InMission);
        return s_Reads;
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
        s_Identity.initial_outfit_id = Field<std::string>::Observed(k_Suit);
        s_Identity.invincible = Field<bool>::Observed(false);
        s_Identity.x = Field<double>::Observed(-0.035);
        s_Identity.y = Field<double>::Observed(-3.832);
        s_Identity.z = Field<double>::Observed(-0.413);
        s_Identity.transform_dirty = Field<bool>::Observed(false);
        s_Identity.room_behaviour = Field<int64_t>::Observed(1);
        return s_Identity;
    }

    LookupFacts GoodLookup(bool p_Same = true)
    {
        LookupFacts s_Facts;
        s_Facts.owner_found = Field<bool>::Observed(true);
        s_Facts.factory_available = Field<bool>::Observed(true);
        s_Facts.factory_template = Field<bool>::Observed(true);
        s_Facts.subentity_count = Field<int64_t>::Observed(2);
        s_Facts.index = Field<int64_t>::Observed(1);
        s_Facts.index_check = Field<std::string>::Observed("in_range");
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

    // The entity read set of a stable single-player frame: 23 FAKE bricks, the hero owned by one of
    // them, the round trip returning the hero, the candidate brick absent.
    EntityReads GoodEntity(size_t p_Bricks = 23)
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
        s_Reads.round_trip = GoodLookup(true);
        s_Reads.round_trip_outcome = PlayerObservation::ClassifyRoundTrip(s_Reads.round_trip);

        // the candidate absent from the scanned prefix: "false" from a complete scan, unavailable
        // with the scan bound otherwise (correction C), exactly as the reader decides it
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

    EntityReads WithCandidatePresent(EntityReads p_Reads)
    {
        auto& s_Candidate = *p_Reads.candidate;
        s_Candidate.brick_present = PlayerObservation::CandidatePresence(true, p_Reads.bricks);
        s_Candidate.lookup = GoodLookup(false); // a different entity from the local hero
        s_Candidate.lookup.subentity_count = Field<int64_t>::Observed(3);
        s_Candidate.lookup.index = Field<int64_t>::Observed(0);
        s_Candidate.lookup.index_check = Field<std::string>::Observed("in_range");
        s_Candidate.identity = HeroIdentity(k_Agent48, k_HitmenBrick);
        s_Candidate.identity.x = Field<double>::Observed(-40.99882);
        s_Candidate.identity.y = Field<double>::Observed(-24.293657);
        s_Candidate.identity.z = Field<double>::Observed(0.4925565);
        s_Candidate.hitman5_resolved = Field<bool>::Observed(true);
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

    bool NumberField(const Result& p_Result, const char* p_Key, double p_Expected)
    {
        const auto* s_Value = Observed(p_Result, p_Key);
        return s_Value && s_Value->field_status && *s_Value->field_status == FieldStatus::Observed && s_Value->kind == ObservedValue::Kind::Number && s_Value->number == p_Expected;
    }

    std::string Reason(const Result& p_Result, const char* p_Key)
    {
        const auto* s_Value = Observed(p_Result, p_Key);
        return s_Value ? s_Value->field_reason : std::string();
    }

    size_t CountPrefixed(const Result& p_Result, const char* p_Prefix)
    {
        size_t s_Count = 0;
        for (const auto& s_Entry : p_Result.observed)
            if (s_Entry.first.rfind(p_Prefix, 0) == 0)
                ++s_Count;
        return s_Count;
    }

    // Runs one players inspection on one frame with the given fake inspector.
    Result RunPlayers(const CommandScheduler::FrameObservation& p_Frame, CommandScheduler::PlayersInspector p_Inspector, CommandScheduler::Stats* p_Stats = nullptr)
    {
        CommandQueue s_Queue;
        std::vector<Result> s_Results;
        CommandScheduler s_Scheduler(s_Queue, [&](const Result& p_Result) { s_Results.push_back(p_Result); });
        s_Queue.Push(Players(CommandTest::Uuid(1)), 1, p_Frame.now_mono_ms > 10 ? p_Frame.now_mono_ms - 10 : 0);
        CommandScheduler::FrameObservation s_Frame = p_Frame;
        s_Frame.players_inspector = std::move(p_Inspector);
        CHECK(s_Scheduler.Tick(s_Frame));
        CHECK(s_Results.size() == 1);
        if (p_Stats)
            *p_Stats = s_Scheduler.GetStats();
        return s_Results.empty() ? Result{} : s_Results[0];
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

void RunPlayerInspectionTests()
{
    // ---- hex ids ---------------------------------------------------------------------------------
    {
        CHECK(PlayerObservation::Hex16(0xfeede715906f747full) == "feede715906f747f");
        CHECK(PlayerObservation::Hex16(0) == "0000000000000000");
        uint64_t s_Value = 0;
        CHECK(PlayerObservation::ParseHex16("feede715906f747f", s_Value) && s_Value == 0xfeede715906f747full);
        CHECK(PlayerObservation::ParseHex16("007c57a6ea581f8c", s_Value) && s_Value == 0x007c57a6ea581f8cull);
        CHECK(!PlayerObservation::ParseHex16("FEEDE715906F747F", s_Value));
        CHECK(!PlayerObservation::ParseHex16("feede715906f747", s_Value));
        CHECK(!PlayerObservation::ParseHex16("feede715906f747fa", s_Value));
        CHECK(!PlayerObservation::ValidHex16("0x00000000000000") && PlayerObservation::ValidHex16("0000000000000000"));
    }

    // ---- the registry words classified ----------------------------------------------------------
    {
        const uint64_t s_Base = 0x14313db10ull; // experiment 4's &m_aPlayerData[0]
        CHECK(PlayerObservation::ClassifyWord(s_Base, s_Base, 0xD0, 4) == "slot0");
        CHECK(PlayerObservation::ClassifyWord(s_Base + 3 * 0xD0, s_Base, 0xD0, 4) == "slot3");
        CHECK(PlayerObservation::ClassifyWord(s_Base + 4 * 0xD0, s_Base, 0xD0, 4) == "0x14313de50");      // one past the slots
        CHECK(PlayerObservation::ClassifyWord(s_Base + 8, s_Base, 0xD0, 4) == "0x14313db18");             // inside a slot, not its start
        CHECK(PlayerObservation::ClassifyWord(0, s_Base, 0xD0, 4) == "0");
        CHECK(PlayerObservation::ClassifyWord(0x4000000000000101ull, s_Base, 0xD0, 4) == "0x4000000000000101");
        CHECK(PlayerObservation::ClassifyWord(s_Base - 0xD0, s_Base, 0xD0, 4) == "0x14313da40");          // below the slots
        CHECK(PlayerObservation::ClassifyWord(s_Base, s_Base, 0, 4) == "0x14313db10");                    // a zero slot size never divides
    }

    // ---- brick-id grouping -----------------------------------------------------------------------
    {
        CHECK(PlayerObservation::GroupIds({}).empty());
        const auto s_Seven = PlayerObservation::GroupIds(FakeBrickIds(7));
        CHECK(s_Seven.size() == 1 && s_Seven[0].size() == 7 * 16 + 6 && s_Seven[0].size() <= k_MaxObservedTextBytes);
        const auto s_Eight = PlayerObservation::GroupIds(FakeBrickIds(8));
        CHECK(s_Eight.size() == 2 && s_Eight[1] == "0000000000000008");
        const auto s_Max = PlayerObservation::GroupIds(FakeBrickIds(21));
        CHECK(s_Max.size() == 3 && s_Max[2].size() == 7 * 16 + 6);
        const auto s_Over = PlayerObservation::GroupIds(FakeBrickIds(25));
        CHECK(s_Over.size() == 3 && s_Over[2].find("0000000000000015") != std::string::npos && s_Over[2].find("0000000000000016") == std::string::npos);
        CHECK(PlayerObservation::k_MaxBrickIds == 21);
    }

    // ---- the round-trip classification: the six outcomes, each from the first gap in the chain ----
    {
        CHECK(PlayerObservation::ClassifyRoundTrip(GoodLookup(true)) == RoundTrip::SameEntity);
        CHECK(PlayerObservation::ClassifyRoundTrip(GoodLookup(false)) == RoundTrip::DifferentEntity);

        LookupFacts s_NoOwner = GoodLookup();
        s_NoOwner.owner_found = Field<bool>::Observed(false);
        CHECK(PlayerObservation::ClassifyRoundTrip(s_NoOwner) == RoundTrip::OwnerUnresolved);

        LookupFacts s_NotSub = GoodLookup();
        s_NotSub.index = Field<int64_t>::Observed(-1);
        s_NotSub.index_check = Field<std::string>::Observed("negative");
        s_NotSub.resolved = Field<bool>::Of(FieldStatus::Unavailable, "not a sub-entity of that factory");
        CHECK(PlayerObservation::ClassifyRoundTrip(s_NotSub) == RoundTrip::HeroNotRepresentable);

        LookupFacts s_NoFactory = GoodLookup();
        s_NoFactory.factory_available = Field<bool>::Observed(false);
        CHECK(PlayerObservation::ClassifyRoundTrip(s_NoFactory) == RoundTrip::FactoryUnavailable);

        LookupFacts s_NotTemplate = GoodLookup();
        s_NotTemplate.factory_template = Field<bool>::Observed(false);
        CHECK(PlayerObservation::ClassifyRoundTrip(s_NotTemplate) == RoundTrip::FactoryUnavailable);

        LookupFacts s_NullSub = GoodLookup();
        s_NullSub.resolved = Field<bool>::Observed(false);
        s_NullSub.same_entity = Field<bool>::Of(FieldStatus::Unavailable, "sub-entity null at a valid index");
        CHECK(PlayerObservation::ClassifyRoundTrip(s_NullSub) == RoundTrip::Unsupported);

        LookupFacts s_Unread;
        PlayerObservation::MarkLookup(s_Unread, FieldStatus::Unavailable, "hero entity reference null");
        CHECK(PlayerObservation::ClassifyRoundTrip(s_Unread) == RoundTrip::Unsupported);

        // a different entity is reported only when every earlier step was demonstrably valid
        LookupFacts s_DifferentButNotTemplate = GoodLookup(false);
        s_DifferentButNotTemplate.factory_template = Field<bool>::Observed(false);
        CHECK(PlayerObservation::ClassifyRoundTrip(s_DifferentButNotTemplate) == RoundTrip::FactoryUnavailable);

        CHECK(std::string(PlayerObservation::RoundTripText(RoundTrip::SameEntity)) == "same_entity");
        CHECK(std::string(PlayerObservation::RoundTripText(RoundTrip::DifferentEntity)) == "different_entity");
        CHECK(std::string(PlayerObservation::RoundTripText(RoundTrip::OwnerUnresolved)) == "owner_unresolved");
        CHECK(std::string(PlayerObservation::RoundTripText(RoundTrip::HeroNotRepresentable)) == "hero_not_representable");
        CHECK(std::string(PlayerObservation::RoundTripText(RoundTrip::FactoryUnavailable)) == "factory_unavailable");
        CHECK(std::string(PlayerObservation::RoundTripText(RoundTrip::Unsupported)) == "unsupported");
    }

    // ---- encoding: entry counts and the line bound -----------------------------------------------
    {
        OutfitObservation::Entries s_Registry;
        PlayerObservation::AppendRegistry(s_Registry, SinglePlayer(true));
        CHECK(s_Registry.size() == 42);
        CHECK(s_Registry.front().first == "registry_available" && s_Registry.back().first == "slot0_matches_m5");

        OutfitObservation::Entries s_Entity;
        PlayerObservation::AppendEntity(s_Entity, GoodEntity());
        CHECK(s_Entity.size() == 6 + 14 + 8 + 3); // 31: bricks (count, 3 id fields, truncated, scanned), hero (resolved + 13), round trip (incl. the index check), candidate echo + absent

        OutfitObservation::Entries s_Present;
        PlayerObservation::AppendEntity(s_Present, WithCandidatePresent(GoodEntity()));
        CHECK(s_Present.size() == 31 + 13); // + the candidate's 7 lookup fields and 6 compact identity fields
        CHECK(s_Present.size() + 4 == k_MaxObservedEntries); // plus the scheduler's four frame values: exactly the entry cap

        OutfitObservation::Entries s_Empty;
        PlayerObservation::AppendEntity(s_Empty, GoodEntity(0));
        CHECK(s_Empty.size() == 3 + 14 + 8 + 3); // no brick_ids_ field at all

        // every unavailable field with a full-length reason still fits the line
        Result s_Worst;
        s_Worst.command_id = CommandTest::Uuid(1);
        s_Worst.adapter_instance_id = CommandTest::Uuid(99);
        s_Worst.native_at = "2026-10-15T00:00:00.000Z";
        s_Worst.frame = 123456789;
        s_Worst.mission_generation = k_Gen1;
        s_Worst.status = Status::Applied;
        PlayerObservation::AppendRegistry(s_Worst.observed, PlayerObservation::AllRegistry(FieldStatus::Unavailable, "a reason of exactly forty bytes.........."));
        // 42 unavailable fields with full-length reasons exceed the line; the first reduction stage
        // drops only the reasons and keeps every field and status
        CHECK(EncodeResult(s_Worst).size() > k_MaxLineBytes);
        Result s_WorstReduced = s_Worst;
        CHECK(CommandScheduler::ReduceToFit(s_WorstReduced));
        CHECK(EncodeResult(s_WorstReduced).size() <= k_MaxLineBytes);
        CHECK(s_WorstReduced.observed.size() == 43 && s_WorstReduced.observed.back().first == "reasons_dropped" && !Observed(s_WorstReduced, "observed_reduced"));
        CHECK(FieldIs(s_WorstReduced, "slot3_session", FieldStatus::Unavailable) && Reason(s_WorstReduced, "slot3_session").empty());
        CHECK(s_WorstReduced.reason == "field reasons dropped to fit the result line bound");
        Result s_WorstEntity = s_Worst;
        s_WorstEntity.observed.clear();
        PlayerObservation::AppendEntity(s_WorstEntity.observed, PlayerObservation::AllEntity(FieldStatus::Unavailable, "a reason of exactly forty bytes..........", EntityQuery{k_HitmenBrick, k_Agent48}));
        CHECK(EncodeResult(s_WorstEntity).size() <= k_MaxLineBytes);
        Result s_Longest = s_Worst;
        s_Longest.observed.clear();
        EntityReads s_Long = WithCandidatePresent(GoodEntity(23));
        s_Long.hero.owning_brick = Field<std::string>::Observed(std::string(128, 'f'));
        PlayerObservation::AppendEntity(s_Longest.observed, s_Long);
        CHECK(EncodeResult(s_Longest).size() <= k_MaxLineBytes);

        // reduction: a result over the bound loses entries from the end and says so
        Result s_Over = s_Worst;
        s_Over.observed.clear();
        for (int i = 0; i < 48; ++i)
            OutfitObservation::Append(s_Over.observed, ("field_" + std::to_string(i)).c_str(), Field<std::string>::Observed(std::string(128, 'x')));
        CHECK(EncodeResult(s_Over).size() > k_MaxLineBytes);
        CHECK(CommandScheduler::ReduceToFit(s_Over));
        CHECK(EncodeResult(s_Over).size() <= k_MaxLineBytes);
        CHECK(!s_Over.observed.empty() && s_Over.observed.back().first == "observed_reduced" && s_Over.observed.back().second.boolean);
        CHECK(s_Over.reason.find("reduced") != std::string::npos);
        Result s_Fits = s_Worst;
        s_Fits.observed.clear();
        PlayerObservation::AppendRegistry(s_Fits.observed, SinglePlayer(true));
        CHECK(!CommandScheduler::ReduceToFit(s_Fits) && s_Fits.reason.empty() && s_Fits.observed.size() == 42);
    }

    // ---- relay.inspect.players: a stable mission with one player -------------------------------
    {
        CommandScheduler::Stats s_Stats;
        const Result s_Result = RunPlayers(Frame(100, 5000, true), [] { return SinglePlayer(true); }, &s_Stats);
        CHECK(s_Result.status == Status::Applied && s_Result.reason.empty());
        CHECK(s_Result.frame && *s_Result.frame == 100);
        CHECK(s_Result.mission_generation && *s_Result.mission_generation == k_Gen1);
        CHECK(BoolField(s_Result, "mission_playing", true) && TextField(s_Result, "scene_type") == "mission");
        CHECK(IntField(s_Result, "receipt_to_frame_ms") == 10);
        CHECK(BoolField(s_Result, "registry_available", true));
        CHECK(TextField(s_Result, "registry_words") == "slot0,0,0x4000000000000101");
        CHECK(IntField(s_Result, "slot0_player_id") == 0 && BoolField(s_Result, "slot0_local_flag", true) && BoolField(s_Result, "slot0_connected", false));
        CHECK(BoolField(s_Result, "slot0_net_player", false) && BoolField(s_Result, "slot0_replica", false));
        CHECK(BoolField(s_Result, "slot0_entity", true) && BoolField(s_Result, "slot0_hero", true) && IntField(s_Result, "slot0_flags_a0") == 0);
        CHECK(TextField(s_Result, "slot0_session") == k_Gen1);
        CHECK(TextField(s_Result, "slot0_outfit_id") == k_Suit && TextField(s_Result, "slot0_character_id") == k_Zeros);
        CHECK(TextField(s_Result, "slot0_hero_kit_id") == k_Suit && BoolField(s_Result, "slot0_matches_m5", true));
        for (int i = 1; i <= 3; ++i)
        {
            const std::string s_P = "slot" + std::to_string(i) + "_";
            CHECK(IntField(s_Result, (s_P + "player_id").c_str()) == -1);
            CHECK(BoolField(s_Result, (s_P + "local_flag").c_str(), true)); // not a discriminator: reported as read
            CHECK(BoolField(s_Result, (s_P + "entity").c_str(), false));
            CHECK(FieldIs(s_Result, (s_P + "hero").c_str(), FieldStatus::Unavailable) && Reason(s_Result, (s_P + "hero").c_str()) == "entity reference null");
            CHECK(IntField(s_Result, (s_P + "flags_a0").c_str()) == -1);
            CHECK(FieldIs(s_Result, (s_P + "session").c_str(), FieldStatus::Unavailable));
        }
        CHECK(BoolField(s_Result, "reads_on_frame", true));
        CHECK(s_Result.observed.size() == 46 && s_Result.observed.size() <= k_MaxObservedEntries);
        CHECK(EncodeResult(s_Result).size() <= k_MaxLineBytes);
        CHECK(s_Stats.players_inspected == 1 && s_Stats.entities_inspected == 0 && s_Stats.inspected == 0 && s_Stats.executed == 0 && s_Stats.outfit_dispatched == 0 && s_Stats.reduced == 0);
    }

    // ---- menu-time: a hero resolves, no mission generation, the session empty, the guid zeros ---
    {
        const Result s_Result = RunPlayers(Frame(10, 500, false), [] { return SinglePlayer(false); });
        CHECK(s_Result.status == Status::Applied && !s_Result.mission_generation);
        CHECK(BoolField(s_Result, "mission_playing", false) && TextField(s_Result, "scene_type").empty());
        CHECK(BoolField(s_Result, "slot0_hero", true) && FieldIs(s_Result, "slot0_session", FieldStatus::Unavailable));
        CHECK(TextField(s_Result, "slot0_outfit_id") == k_Zeros && TextField(s_Result, "slot0_hero_kit_id") == k_Suit && BoolField(s_Result, "slot0_matches_m5", false));
    }

    // ---- registry unavailable: applied, every registry field unavailable with the reason --------
    {
        const Result s_Result = RunPlayers(Frame(10, 500, true), [] {
            auto s_Reads = PlayerObservation::AllRegistry(FieldStatus::Unavailable, "player registry global null");
            s_Reads.registry_available = Field<bool>::Observed(false);
            s_Reads.thread_id = GetCurrentThreadId();
            return s_Reads;
        });
        CHECK(s_Result.status == Status::Applied);
        CHECK(BoolField(s_Result, "registry_available", false));
        CHECK(FieldIs(s_Result, "registry_words", FieldStatus::Unavailable) && Reason(s_Result, "registry_words") == "player registry global null");
        CHECK(FieldIs(s_Result, "slot0_player_id", FieldStatus::Unavailable) && FieldIs(s_Result, "slot3_session", FieldStatus::Unavailable) && FieldIs(s_Result, "slot0_matches_m5", FieldStatus::Unavailable));
    }

    // ---- registry layout unsupported: the reader refuses to interpret, nothing is guessed ---------
    {
        const Result s_Result = RunPlayers(Frame(10, 500, true), [] {
            auto s_Reads = PlayerObservation::AllRegistry(FieldStatus::Unsupported, "registry layout unsupported");
            s_Reads.registry_available = Field<bool>::Observed(true);
            s_Reads.thread_id = GetCurrentThreadId();
            return s_Reads;
        });
        CHECK(s_Result.status == Status::Applied && BoolField(s_Result, "registry_available", true));
        CHECK(FieldIs(s_Result, "registry_words", FieldStatus::Unsupported) && FieldIs(s_Result, "slot0_hero", FieldStatus::Unsupported) && FieldIs(s_Result, "slot0_hero_kit_id", FieldStatus::Unsupported));
        CHECK(Reason(s_Result, "slot2_player_id") == "registry layout unsupported");
        CHECK(!Observed(s_Result, "slot0_player_id")->field_status || Observed(s_Result, "slot0_player_id")->kind == ObservedValue::Kind::Null);
    }

    // ---- an additional occupied slot: reported as read, never promoted to a player identity ------
    {
        const Result s_Result = RunPlayers(Frame(10, 500, true), [] {
            auto s_Reads = SinglePlayer(true);
            s_Reads.slots[1] = LocalSlot(true, k_Gen1);
            s_Reads.slots[1].player_id = Field<int64_t>::Observed(2);
            s_Reads.slots[1].local_flag = Field<bool>::Observed(true);
            s_Reads.slots[1].flags_a0 = Field<int64_t>::Observed(0);
            s_Reads.slots[1].net_player = Field<bool>::Observed(true);
            s_Reads.slots[1].connected_flag = Field<bool>::Observed(true);
            s_Reads.slots[1].character_id = Field<std::string>::Observed("787c1ff2-467c-421d-98f4-6bbb92e5fc99");
            return s_Reads;
        });
        CHECK(IntField(s_Result, "slot1_player_id") == 2 && BoolField(s_Result, "slot1_entity", true) && BoolField(s_Result, "slot1_hero", true));
        CHECK(BoolField(s_Result, "slot1_net_player", true) && BoolField(s_Result, "slot1_connected", true) && IntField(s_Result, "slot1_flags_a0") == 0);
        CHECK(IntField(s_Result, "slot0_player_id") == 0 && IntField(s_Result, "slot2_player_id") == -1);
        // the slot-0 extras are the only guid fields: no slot-1 guid is promoted to an identity
        CHECK(!Observed(s_Result, "slot1_character_id") && !Observed(s_Result, "slot1_outfit_id"));
        CHECK(s_Result.observed.size() == 46);
    }

    // ---- the hero interface missing on slot 0: entity present, hero false, M5's path unavailable --
    {
        const Result s_Result = RunPlayers(Frame(10, 500, true), [] {
            auto s_Reads = SinglePlayer(true);
            s_Reads.slots[0].hero_resolved = Field<bool>::Observed(false);
            s_Reads.hero_kit_id = Field<std::string>::Of(FieldStatus::Unavailable, "hero interface not resolved");
            s_Reads.slot0_matches_m5 = Field<bool>::Of(FieldStatus::Unavailable, "kit id or registry outfit guid unavailable");
            return s_Reads;
        });
        CHECK(BoolField(s_Result, "slot0_entity", true) && BoolField(s_Result, "slot0_hero", false));
        CHECK(FieldIs(s_Result, "slot0_hero_kit_id", FieldStatus::Unavailable) && FieldIs(s_Result, "slot0_matches_m5", FieldStatus::Unavailable));
        CHECK(TextField(s_Result, "slot0_outfit_id") == k_Suit); // the registry's own guid is still a read
    }

    // ---- a mission restart: the generation change is in both the result header and the slot -----
    {
        CommandQueue s_Queue;
        std::vector<Result> s_Results;
        CommandScheduler s_Scheduler(s_Queue, [&](const Result& p_Result) { s_Results.push_back(p_Result); });
        std::string s_Generation = k_Gen1;
        auto s_Inspector = [&] { return SinglePlayer(true, s_Generation.c_str()); };

        s_Queue.Push(Players(CommandTest::Uuid(1)), 1, 90);
        CommandScheduler::FrameObservation s_Before = Frame(100, 100, true, k_Gen1);
        s_Before.players_inspector = s_Inspector;
        CHECK(s_Scheduler.Tick(s_Before));

        s_Generation = k_Gen2; // the restart: a new attempt, a new registry session id
        s_Queue.Push(Players(CommandTest::Uuid(2)), 1, 190);
        CommandScheduler::FrameObservation s_After = Frame(200, 200, true, k_Gen2);
        s_After.relay_transition_epoch = 3;
        s_After.players_inspector = s_Inspector;
        CHECK(s_Scheduler.Tick(s_After));

        CHECK(s_Results.size() == 2);
        CHECK(s_Results[0].mission_generation && *s_Results[0].mission_generation == k_Gen1 && TextField(s_Results[0], "slot0_session") == k_Gen1);
        CHECK(s_Results[1].mission_generation && *s_Results[1].mission_generation == k_Gen2 && TextField(s_Results[1], "slot0_session") == k_Gen2);
        CHECK(*s_Results[0].frame == 100 && *s_Results[1].frame == 200);
        CHECK(s_Scheduler.GetStats().players_inspected == 2);
    }

    // ---- no inspector on this frame source: every registry field unsupported, still applied ------
    {
        const Result s_Result = RunPlayers(Frame(10, 500, true), nullptr);
        CHECK(s_Result.status == Status::Applied && s_Result.reason.find("unsupported") != std::string::npos);
        CHECK(FieldIs(s_Result, "registry_available", FieldStatus::Unsupported) && FieldIs(s_Result, "slot0_hero", FieldStatus::Unsupported) && FieldIs(s_Result, "reads_on_frame", FieldStatus::Unsupported));
        CHECK(BoolField(s_Result, "mission_playing", true));
        CHECK(EncodeResult(s_Result).size() <= k_MaxLineBytes);
        // 46 unsupported fields with their reasons exceed the line: the reasons were dropped, every
        // field and status kept, and the result says so beside its own reason
        CHECK(Observed(s_Result, "reasons_dropped") && !Observed(s_Result, "observed_reduced") && Observed(s_Result, "slot3_session"));
        CHECK(s_Result.reason.find("field reasons dropped") != std::string::npos);
    }

    // ---- the reads must be on the frame thread: another thread's reads are reported as such ------
    {
        const Result s_Result = RunPlayers(Frame(10, 500, true), [] {
            auto s_Reads = SinglePlayer(true);
            s_Reads.thread_id = GetCurrentThreadId() + 1;
            return s_Reads;
        });
        CHECK(BoolField(s_Result, "reads_on_frame", false));
    }

    // ---- relay.inspect.entity: the stable single-player frame, the candidate absent -------------
    {
        CommandScheduler::Stats s_Stats;
        EntityQuery s_Seen;
        const Result s_Result = RunEntity(Frame(100, 5000, true), [&](const EntityQuery& p_Query) { s_Seen = p_Query; return GoodEntity(); }, EntityCommand(CommandTest::Uuid(1)), &s_Stats);
        CHECK(!s_Seen.brick && !s_Seen.entity_id); // no parameters: the reader applies the historical defaults
        CHECK(s_Result.status == Status::Applied && s_Result.reason.empty());
        CHECK(IntField(s_Result, "brick_count") == 23 && CountPrefixed(s_Result, "brick_ids_") == 3 && BoolField(s_Result, "bricks_truncated", true) && IntField(s_Result, "bricks_scanned") == 23);
        CHECK(TextField(s_Result, "brick_ids_1").size() == 7 * 16 + 6);
        CHECK(BoolField(s_Result, "hero_resolved", true) && TextField(s_Result, "hero_entity_id") == k_HeroId && IntField(s_Result, "hero_entity_flags") == 7);
        CHECK(BoolField(s_Result, "hero_activated", true) && BoolField(s_Result, "hero_predeleted", false));
        CHECK(TextField(s_Result, "hero_owning_brick") == k_HeroBrick);
        CHECK(TextField(s_Result, "hero_character_id") == "787c1ff2-467c-421d-98f4-6bbb92e5fc99" && TextField(s_Result, "hero_initial_outfit_id") == k_Suit && BoolField(s_Result, "hero_invincible", false));
        CHECK(NumberField(s_Result, "hero_x", -0.035) && NumberField(s_Result, "hero_y", -3.832) && NumberField(s_Result, "hero_z", -0.413));
        CHECK(BoolField(s_Result, "hero_transform_dirty", false) && IntField(s_Result, "hero_room_behaviour") == 1);
        CHECK(TextField(s_Result, "rt_outcome") == "same_entity" && BoolField(s_Result, "rt_owner_found", true) && BoolField(s_Result, "rt_factory_template", true));
        CHECK(IntField(s_Result, "rt_subentity_count") == 2 && IntField(s_Result, "rt_index") == 1 && TextField(s_Result, "rt_index_check") == "in_range" && BoolField(s_Result, "rt_resolved", true) && BoolField(s_Result, "rt_same_entity", true));
        CHECK(TextField(s_Result, "cand_brick") == k_HitmenBrick && TextField(s_Result, "cand_entity_id") == k_Agent48 && BoolField(s_Result, "cand_brick_present", false));
        CHECK(!Observed(s_Result, "cand_index") && !Observed(s_Result, "cand_index_check") && !Observed(s_Result, "cand_distinct_from_local") && !Observed(s_Result, "cand_x"));
        CHECK(BoolField(s_Result, "reads_on_frame", true));
        CHECK(s_Result.observed.size() == 35 && EncodeResult(s_Result).size() <= k_MaxLineBytes);
        CHECK(s_Stats.entities_inspected == 1 && s_Stats.players_inspected == 0 && s_Stats.reduced == 0);
    }

    // ---- the parameters reach the reader as the query; an absent one stays absent ---------------
    {
        EntityQuery s_Seen;
        RunEntity(Frame(100, 5000, true), [&](const EntityQuery& p_Query) { s_Seen = p_Query; return GoodEntity(); }, EntityCommand(CommandTest::Uuid(1), std::string("0012e5a7b1c9d3ee"), std::nullopt));
        CHECK(s_Seen.brick && *s_Seen.brick == "0012e5a7b1c9d3ee" && !s_Seen.entity_id);
        RunEntity(Frame(100, 5000, true), [&](const EntityQuery& p_Query) { s_Seen = p_Query; return GoodEntity(); }, EntityCommand(CommandTest::Uuid(2), std::nullopt, std::string(k_Agent48)));
        CHECK(!s_Seen.brick && s_Seen.entity_id && *s_Seen.entity_id == k_Agent48);
    }

    // ---- the candidate present: the lookup facts, the identity, distinct from the local hero -----
    {
        const Result s_Result = RunEntity(Frame(100, 5000, true), [](const EntityQuery&) { return WithCandidatePresent(GoodEntity()); }, EntityCommand(CommandTest::Uuid(1)));
        CHECK(BoolField(s_Result, "cand_brick_present", true) && BoolField(s_Result, "cand_factory_template", true));
        CHECK(IntField(s_Result, "cand_subentity_count") == 3 && IntField(s_Result, "cand_index") == 0 && TextField(s_Result, "cand_index_check") == "in_range" && BoolField(s_Result, "cand_resolved", true));
        CHECK(BoolField(s_Result, "cand_hitman5", true) && !Observed(s_Result, "cand_spatial") && BoolField(s_Result, "cand_distinct_from_local", true));
        CHECK(TextField(s_Result, "cand_entity_id") == k_Agent48 && IntField(s_Result, "cand_entity_flags") == 7 && TextField(s_Result, "cand_character_id") == "787c1ff2-467c-421d-98f4-6bbb92e5fc99");
        CHECK(CountPrefixed(s_Result, "cand_entity_id") == 1); // the echo once; the compact identity does not repeat it
        CHECK(NumberField(s_Result, "cand_x", -40.99882) && NumberField(s_Result, "cand_y", -24.293657) && NumberField(s_Result, "cand_z", 0.4925565) && BoolField(s_Result, "cand_transform_dirty", false));
        CHECK(!Observed(s_Result, "cand_activated") && !Observed(s_Result, "cand_owning_brick")); // the compact identity
        CHECK(s_Result.observed.size() == 35 + 13 && s_Result.observed.size() == k_MaxObservedEntries);
        CHECK(EncodeResult(s_Result).size() <= k_MaxLineBytes && s_Result.reason.empty());

        // the candidate resolving to the local hero's own entity is reported as not distinct
        const Result s_Same = RunEntity(Frame(100, 5000, true), [](const EntityQuery&) {
            auto s_Reads = WithCandidatePresent(GoodEntity());
            s_Reads.candidate->lookup.same_entity = Field<bool>::Observed(true);
            return s_Reads;
        }, EntityCommand(CommandTest::Uuid(2)));
        CHECK(BoolField(s_Same, "cand_distinct_from_local", false));
    }

    // ---- the round trip returns a different entity: the one specific failure of the contract ----
    {
        const Result s_Result = RunEntity(Frame(100, 5000, true), [](const EntityQuery&) {
            auto s_Reads = GoodEntity();
            s_Reads.round_trip = GoodLookup(false);
            s_Reads.round_trip_outcome = PlayerObservation::ClassifyRoundTrip(s_Reads.round_trip);
            return s_Reads;
        }, EntityCommand(CommandTest::Uuid(1)));
        CHECK(s_Result.status == Status::Applied); // an inspection is applied whatever it found
        CHECK(TextField(s_Result, "rt_outcome") == "different_entity" && BoolField(s_Result, "rt_same_entity", false) && BoolField(s_Result, "rt_resolved", true));
    }

    // ---- the hero without an owning entity: owner unresolved, nothing further attempted ---------
    {
        const Result s_Result = RunEntity(Frame(100, 5000, true), [](const EntityQuery&) {
            auto s_Reads = GoodEntity();
            s_Reads.hero.owning_brick = Field<std::string>::Of(FieldStatus::Unavailable, "no owning entity");
            PlayerObservation::MarkLookup(s_Reads.round_trip, FieldStatus::Unavailable, "owner not found");
            s_Reads.round_trip.owner_found = Field<bool>::Observed(false);
            s_Reads.round_trip_outcome = PlayerObservation::ClassifyRoundTrip(s_Reads.round_trip);
            return s_Reads;
        }, EntityCommand(CommandTest::Uuid(1)));
        CHECK(TextField(s_Result, "rt_outcome") == "owner_unresolved" && BoolField(s_Result, "rt_owner_found", false));
        CHECK(FieldIs(s_Result, "rt_index", FieldStatus::Unavailable) && FieldIs(s_Result, "hero_owning_brick", FieldStatus::Unavailable));
    }

    // ---- a factory type mismatch: no lookup call is made past the type check ------------------
    {
        const Result s_Result = RunEntity(Frame(100, 5000, true), [](const EntityQuery&) {
            auto s_Reads = GoodEntity();
            s_Reads.round_trip.factory_template = Field<bool>::Observed(false);
            s_Reads.round_trip.subentity_count = Field<int64_t>::Of(FieldStatus::Unavailable, "not a template entity blueprint factory");
            s_Reads.round_trip.index = Field<int64_t>::Of(FieldStatus::Unavailable, "not a template entity blueprint factory");
            s_Reads.round_trip.index_check = Field<std::string>::Of(FieldStatus::Unavailable, "not a template entity blueprint factory");
            s_Reads.round_trip.resolved = Field<bool>::Of(FieldStatus::Unavailable, "not a template entity blueprint factory");
            s_Reads.round_trip.same_entity = Field<bool>::Of(FieldStatus::Unavailable, "not a template entity blueprint factory");
            s_Reads.round_trip_outcome = PlayerObservation::ClassifyRoundTrip(s_Reads.round_trip);
            return s_Reads;
        }, EntityCommand(CommandTest::Uuid(1)));
        CHECK(TextField(s_Result, "rt_outcome") == "factory_unavailable" && BoolField(s_Result, "rt_factory_template", false) && FieldIs(s_Result, "rt_subentity_count", FieldStatus::Unavailable) && FieldIs(s_Result, "rt_index_check", FieldStatus::Unavailable));
    }

    // ---- the hero's id is not a sub-entity of its owner's factory; and a null sub-entity ---------
    {
        const Result s_Missing = RunEntity(Frame(100, 5000, true), [](const EntityQuery&) {
            auto s_Reads = GoodEntity();
            s_Reads.round_trip.index = Field<int64_t>::Observed(-1);
            s_Reads.round_trip.index_check = Field<std::string>::Observed("negative");
            s_Reads.round_trip.resolved = Field<bool>::Of(FieldStatus::Unavailable, "not a sub-entity of that factory");
            s_Reads.round_trip.same_entity = Field<bool>::Of(FieldStatus::Unavailable, "not a sub-entity of that factory");
            s_Reads.round_trip_outcome = PlayerObservation::ClassifyRoundTrip(s_Reads.round_trip);
            return s_Reads;
        }, EntityCommand(CommandTest::Uuid(1)));
        CHECK(TextField(s_Missing, "rt_outcome") == "hero_not_representable" && IntField(s_Missing, "rt_index") == -1 && TextField(s_Missing, "rt_index_check") == "negative");

        const Result s_Null = RunEntity(Frame(100, 5000, true), [](const EntityQuery&) {
            auto s_Reads = GoodEntity();
            s_Reads.round_trip.resolved = Field<bool>::Observed(false);
            s_Reads.round_trip.same_entity = Field<bool>::Of(FieldStatus::Unavailable, "sub-entity null at a valid index");
            s_Reads.round_trip_outcome = PlayerObservation::ClassifyRoundTrip(s_Reads.round_trip);
            return s_Reads;
        }, EntityCommand(CommandTest::Uuid(2)));
        CHECK(TextField(s_Null, "rt_outcome") == "unsupported" && BoolField(s_Null, "rt_resolved", false));
    }

    // ---- a null hero reference: everything about the hero unavailable, the bricks still read ----
    {
        const Result s_Result = RunEntity(Frame(10, 500, false), [](const EntityQuery&) {
            auto s_Reads = GoodEntity(5);
            PlayerObservation::MarkIdentity(s_Reads.hero, FieldStatus::Unavailable, "hero entity reference null");
            PlayerObservation::MarkLookup(s_Reads.round_trip, FieldStatus::Unavailable, "hero entity reference null");
            s_Reads.round_trip_outcome = PlayerObservation::ClassifyRoundTrip(s_Reads.round_trip);
            return s_Reads;
        }, EntityCommand(CommandTest::Uuid(1)));
        CHECK(s_Result.status == Status::Applied && !s_Result.mission_generation);
        CHECK(IntField(s_Result, "brick_count") == 5 && CountPrefixed(s_Result, "brick_ids_") == 1 && BoolField(s_Result, "bricks_truncated", false) && IntField(s_Result, "bricks_scanned") == 5);
        CHECK(FieldIs(s_Result, "hero_resolved", FieldStatus::Unavailable) && FieldIs(s_Result, "hero_entity_id", FieldStatus::Unavailable) && FieldIs(s_Result, "hero_x", FieldStatus::Unavailable));
        CHECK(Reason(s_Result, "hero_entity_id") == "hero entity reference null");
        CHECK(TextField(s_Result, "rt_outcome") == "unsupported" && FieldIs(s_Result, "rt_owner_found", FieldStatus::Unavailable) && FieldIs(s_Result, "rt_index_check", FieldStatus::Unavailable));
    }

    // ---- a dirty transform and invalid flags are reported, never corrected ----------------------
    {
        const Result s_Result = RunEntity(Frame(100, 5000, true), [](const EntityQuery&) {
            auto s_Reads = GoodEntity();
            s_Reads.hero.transform_dirty = Field<bool>::Observed(true);
            s_Reads.hero.entity_flags = Field<int64_t>::Observed(8 | 2);
            s_Reads.hero.activated = Field<bool>::Observed(false);
            s_Reads.hero.predeleted = Field<bool>::Observed(true);
            return s_Reads;
        }, EntityCommand(CommandTest::Uuid(1)));
        CHECK(BoolField(s_Result, "hero_transform_dirty", true) && NumberField(s_Result, "hero_x", -0.035));
        CHECK(IntField(s_Result, "hero_entity_flags") == 10 && BoolField(s_Result, "hero_activated", false) && BoolField(s_Result, "hero_predeleted", true));
    }

    // ---- empty and oversized brick lists ---------------------------------------------------------
    {
        const Result s_Empty = RunEntity(Frame(100, 5000, true), [](const EntityQuery&) { return GoodEntity(0); }, EntityCommand(CommandTest::Uuid(1)));
        CHECK(IntField(s_Empty, "brick_count") == 0 && CountPrefixed(s_Empty, "brick_ids_") == 0 && BoolField(s_Empty, "bricks_truncated", false) && IntField(s_Empty, "bricks_scanned") == 0);
        CHECK(BoolField(s_Empty, "cand_brick_present", false)); // an empty list is a complete scan
        // beyond the scan bound: the display is truncated, the scan is partial, and the candidate's
        // absence is not established (correction C)
        const Result s_Over = RunEntity(Frame(100, 5000, true), [](const EntityQuery&) { return GoodEntity(70); }, EntityCommand(CommandTest::Uuid(2)));
        CHECK(IntField(s_Over, "brick_count") == 70 && CountPrefixed(s_Over, "brick_ids_") == 3 && BoolField(s_Over, "bricks_truncated", true) && IntField(s_Over, "bricks_scanned") == 64);
        CHECK(TextField(s_Over, "brick_ids_3").find("0000000000000015") != std::string::npos && TextField(s_Over, "brick_ids_3").find("0000000000000016") == std::string::npos);
        CHECK(FieldIs(s_Over, "cand_brick_present", FieldStatus::Unavailable) && Reason(s_Over, "cand_brick_present") == "scan bound: 64/70 bricks scanned" && !Observed(s_Over, "cand_index"));
        CHECK(EncodeResult(s_Over).size() <= k_MaxLineBytes);
    }

    // ---- no inspector: the entity fields unsupported, the candidate echoed, still applied --------
    {
        const Result s_Result = RunEntity(Frame(100, 5000, true), nullptr, EntityCommand(CommandTest::Uuid(1), std::string(k_HitmenBrick), std::string(k_Agent48)));
        CHECK(s_Result.status == Status::Applied && s_Result.reason.find("unsupported") != std::string::npos);
        CHECK(FieldIs(s_Result, "brick_count", FieldStatus::Unsupported) && FieldIs(s_Result, "hero_resolved", FieldStatus::Unsupported) && TextField(s_Result, "rt_outcome") == "unsupported");
        CHECK(TextField(s_Result, "cand_brick") == k_HitmenBrick && TextField(s_Result, "cand_entity_id") == k_Agent48 && FieldIs(s_Result, "cand_brick_present", FieldStatus::Unsupported));
        CHECK(FieldIs(s_Result, "reads_on_frame", FieldStatus::Unsupported));
    }

    // ---- the unavailable scene: both inspections report the scene type unavailable, still read ---
    {
        const Result s_Result = RunPlayers(Frame(10, 500, false, std::nullopt, false), [] { return SinglePlayer(false); });
        CHECK(s_Result.status == Status::Applied && FieldIs(s_Result, "scene_type", FieldStatus::Unavailable) && BoolField(s_Result, "slot0_hero", true));
    }

    // ---- the link generation and TTL rules apply to the new entries exactly as to relay.probe ----
    {
        CommandQueue s_Queue;
        std::vector<Result> s_Results;
        CommandScheduler s_Scheduler(s_Queue, [&](const Result& p_Result) { s_Results.push_back(p_Result); });
        s_Queue.Push(Players(CommandTest::Uuid(1), 1, 100), 1, 1000);
        CommandScheduler::FrameObservation s_Late = Frame(1, 1101, true);
        s_Late.players_inspector = [] { return SinglePlayer(true); };
        CHECK(s_Scheduler.Tick(s_Late));
        CHECK(s_Results.size() == 1 && s_Results[0].status == Status::Expired && s_Scheduler.GetStats().players_inspected == 0);

        s_Queue.Push(EntityCommand(CommandTest::Uuid(2)), 1, 2000);
        CommandScheduler::FrameObservation s_Stale = Frame(2, 2010, true);
        s_Stale.link_generation = 2;
        s_Stale.entity_inspector = [](const EntityQuery&) { return GoodEntity(); };
        CHECK(s_Scheduler.Tick(s_Stale));
        CHECK(s_Results.size() == 1 && s_Scheduler.GetStats().stale_generation == 1 && s_Scheduler.GetStats().entities_inspected == 0);
    }

    // ---- no engine setter exists on this path: the result never carries an apply field ----------
    {
        const Result s_Players = RunPlayers(Frame(100, 5000, true), [] { return SinglePlayer(true); });
        const Result s_Entity = RunEntity(Frame(100, 5000, true), [](const EntityQuery&) { return WithCandidatePresent(GoodEntity()); }, EntityCommand(CommandTest::Uuid(1)));
        for (const Result* s_Result : {&s_Players, &s_Entity})
        {
            CHECK(!Observed(*s_Result, "setter_called") && !Observed(*s_Result, "invocation") && !Observed(*s_Result, "effect") && !Observed(*s_Result, "apply_log"));
            CHECK(!IsEffectful(Entry::RelayInspectPlayers1) && !IsEffectful(Entry::RelayInspectEntity1));
        }
    }
}
