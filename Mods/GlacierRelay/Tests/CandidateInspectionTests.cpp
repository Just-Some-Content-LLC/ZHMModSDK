// M6-D1a tests: the pure rules of CandidateObservation (the packed-group grammar, the member states,
// the slot classification, the knowledge rule, the number rendering), the deterministic encoding of
// the candidate read set and its bounds, and the scheduling of relay.inspect.candidate v1 with FAKE
// inspectors over fake engine-independent observation inputs: no inspector; the candidate brick
// absent (complete and incomplete scans); the candidate entity missing; the candidate hero
// interface missing; the candidate spawn missing; the spawn referencing the candidate and a
// different entity; an invalid factory count; a negative index; an index equal to the count; the
// parent absent and the parent reference unavailable; a dirty world transform; visible and hidden
// spatial flags; the simulation members all present and partially missing; m_bActive unavailable,
// true and false; registry slot 0 matching the local hero; the candidate matching another slot and
// no slot; a mission generation change; a scene unload; the result reduction at the line bound; and
// the absence of any engine setter. Fake values only: no engine pointer is constructed, valid or
// invalid; no memory is written; no setter exists on this path; nothing here is HITMAN state.
#include "TestHarness.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <Windows.h>

#include "CandidateObservation.h"
#include "CommandScheduler.h"
#include "TestListener.h"

using namespace CommandProtocol;
using OutfitObservation::Field;
using CandidateObservation::CandidateKnowledge;
using CandidateObservation::CandidateQuery;
using CandidateObservation::CandidateReads;
using CandidateObservation::HeroControllers;
using CandidateObservation::HeroMembers;
using CandidateObservation::HeroReads;
using CandidateObservation::LocalTransform;
using CandidateObservation::Member;
// (CandidateObservation::OutfitTuple is spelled out: CommandProtocol::OutfitTuple is in scope too)
using CandidateObservation::SlotIdentity;
using CandidateObservation::SlotMatch;
using CandidateObservation::SpatialFlags;
using CandidateObservation::SpawnReads;
using CandidateObservation::Vec3;

namespace
{
    constexpr const char* k_Suit = "874c4c48-0a8b-49e9-883e-49fc5f1fb051";
    constexpr const char* k_Gen1 = "2516103582917844185-a105ec5f-3a75-46a3-8ecd-e72fd15b176c";
    constexpr const char* k_Gen2 = "2516103582027577704-30ccebb9-8138-4c72-9eb9-1a289b4fba33";
    constexpr const char* k_HitmenBrick = "007c57a6ea581f8c"; // a FAKE stand-in for the historical brick id
    constexpr const char* k_Agent48 = "feede715906f747f";
    constexpr const char* k_Agent48Spawn = "feed755acdd56780";
    constexpr const char* k_HeroId = "feed9a0d70d546f4";      // a FAKE authored id for the local hero
    constexpr const char* k_HeroBrick = "0012e5a7b1c9d3ee";   // a FAKE loaded brick that owns the hero
    constexpr const char* k_OtherId = "feed000000000bad";     // a FAKE authored id that is neither
    constexpr const char* k_SceneRootId = "0000000000000001"; // a FAKE parent id

    Command CandidateCommand(const std::string& p_Id, std::optional<std::string> p_Brick = std::nullopt, std::optional<std::string> p_Entity = std::nullopt)
    {
        Command s_Command;
        s_Command.command_id = p_Id;
        s_Command.link_generation = 1;
        s_Command.ttl_ms = k_MaxTtlMs;
        s_Command.name = "relay.inspect.candidate";
        s_Command.version = 1;
        s_Command.entry = Entry::RelayInspectCandidate1;
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

    // ---- fake read sets --------------------------------------------------------------------------

    HeroReads FakeHero(double p_X, double p_Y, double p_Z, bool p_Complete = true)
    {
        HeroReads s_Hero;
        Vec3 s_World;
        s_World.x = p_X;
        s_World.y = p_Y;
        s_World.z = p_Z;
        s_Hero.spatial.world = Field<Vec3>::Observed(s_World);
        LocalTransform s_Local;
        s_Local.translation.x = 0;
        s_Local.translation.y = 0;
        s_Local.translation.z = 0;
        s_Local.rotation.x = 0;
        s_Local.rotation.y = 0;
        s_Local.rotation.z = 0;
        s_Local.rotation.w = 1;
        s_Hero.spatial.local = Field<LocalTransform>::Observed(s_Local);
        s_Hero.spatial.parent = Field<std::string>::Observed(k_SceneRootId);
        SpatialFlags s_Flags;
        s_Flags.visible = true;
        s_Flags.dynamic = true;
        s_Flags.room_behaviour = 1;
        s_Flags.transform_parent = true;
        s_Flags.transform_children = true;
        s_Hero.spatial.flags = Field<SpatialFlags>::Observed(s_Flags);

        HeroMembers s_Members;
        s_Members.character = p_Complete ? Member::Valid : Member::Null;
        s_Members.animator = Member::Present;
        s_Members.morpheme = Member::Present;
        s_Members.ragdoll = p_Complete ? Member::Valid : Member::Null;
        s_Members.health = p_Complete ? Member::Present : Member::Null;
        s_Members.outfit_kit = Member::Valid;
        s_Members.main_camera = p_Complete ? Member::Valid : Member::Null;
        s_Members.input_processor = p_Complete ? Member::Present : Member::Null;
        s_Hero.members = Field<HeroMembers>::Observed(s_Members);

        HeroControllers s_Controllers;
        s_Controllers.guide = p_Complete ? Member::Valid : Member::Null;
        s_Controllers.ik = p_Complete ? Member::Valid : Member::Present;
        s_Controllers.cheat = p_Complete ? Member::Valid : Member::Null;
        s_Controllers.interaction = p_Complete ? Member::Valid : Member::Null;
        s_Controllers.actor_tag = p_Complete ? Member::Valid : Member::Null;
        s_Hero.controllers = Field<HeroControllers>::Observed(s_Controllers);

        CandidateObservation::OutfitTuple s_Outfit;
        s_Outfit.kit_id = k_Suit;
        s_Outfit.charset = 0;
        s_Outfit.variation = 0;
        s_Hero.outfit = Field<CandidateObservation::OutfitTuple>::Observed(s_Outfit);
        s_Hero.active = Field<bool>::Observed(true);
        return s_Hero;
    }

    PlayerObservation::LookupFacts GoodLookup(int64_t p_Count, int64_t p_Index, bool p_Same)
    {
        PlayerObservation::LookupFacts s_Facts;
        s_Facts.owner_found = Field<bool>::Observed(true);
        s_Facts.factory_available = Field<bool>::Observed(true);
        s_Facts.factory_template = Field<bool>::Observed(true);
        s_Facts.subentity_count = Field<int64_t>::Observed(p_Count);
        s_Facts.index = Field<int64_t>::Observed(p_Index);
        s_Facts.index_check = Field<std::string>::Observed(PlayerObservation::IndexCheckText(PlayerObservation::CheckSubEntityIndex(p_Count, p_Index)));
        s_Facts.resolved = Field<bool>::Observed(true);
        s_Facts.same_entity = Field<bool>::Observed(p_Same);
        return s_Facts;
    }

    // A lookup whose factory answer is the given count and index, with the DLL's own refusal rules
    // applied (correction A), as the reader would report it.
    PlayerObservation::LookupFacts LookupWith(int64_t p_Count, int64_t p_Index)
    {
        using PlayerObservation::IndexCheck;
        PlayerObservation::LookupFacts s_Facts;
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
        s_Facts.index = Field<int64_t>::Observed(p_Index);
        const IndexCheck s_Check = PlayerObservation::CheckSubEntityIndex(p_Count, p_Index);
        s_Facts.index_check = Field<std::string>::Observed(PlayerObservation::IndexCheckText(s_Check));
        if (s_Check == IndexCheck::Negative)
        {
            s_Facts.resolved = Field<bool>::Of(FieldStatus::Unavailable, "not a sub-entity of that factory");
            s_Facts.same_entity = Field<bool>::Of(FieldStatus::Unavailable, "not a sub-entity of that factory");
        }
        else if (s_Check != IndexCheck::InRange)
        {
            s_Facts.resolved = Field<bool>::Of(FieldStatus::Inconsistent, "index outside the sub-entity count");
            s_Facts.same_entity = Field<bool>::Of(FieldStatus::Inconsistent, "index outside the sub-entity count");
        }
        else
        {
            s_Facts.resolved = Field<bool>::Observed(true);
            s_Facts.same_entity = Field<bool>::Observed(false);
        }
        return s_Facts;
    }

    SpawnReads FakeSpawn(const char* p_RefId, bool p_RefIsCandidate)
    {
        SpawnReads s_Spawn;
        s_Spawn.lookup = GoodLookup(3, 2, false);
        s_Spawn.class_name = Field<std::string>::Observed("ZHeroSpawn");
        Vec3 s_World;
        s_World.x = -40.99882;
        s_World.y = -24.293657;
        s_World.z = 0.4925565;
        s_Spawn.world = Field<Vec3>::Observed(s_World);
        s_Spawn.dirty = Field<bool>::Observed(false);
        s_Spawn.player_id = Field<int64_t>::Observed(2);
        s_Spawn.default_spawn = Field<bool>::Observed(false);
        s_Spawn.preferred_spawn = Field<bool>::Observed(false);
        s_Spawn.hitman_ref = Field<std::string>::Observed(p_RefId);
        s_Spawn.ref_is_candidate = Field<bool>::Observed(p_RefIsCandidate);
        return s_Spawn;
    }

    void SetSlots(CandidateReads& p_Reads, SlotMatch p_0, SlotMatch p_1, SlotMatch p_2, SlotMatch p_3, bool p_Hero1 = true)
    {
        SlotIdentity s_Slots[4];
        s_Slots[0].match = p_0;
        s_Slots[0].hero_resolved = true;
        s_Slots[1].match = p_1;
        s_Slots[1].hero_resolved = p_Hero1;
        s_Slots[2].match = p_2;
        s_Slots[3].match = p_3;
        p_Reads.registry_slots = Field<std::string>::Observed(CandidateObservation::EncodeSlots(s_Slots));
    }

    // The read set of a stable Paris frame with the candidate present, resolved, complete-looking,
    // its spawn referencing it, the registry holding only the local hero: the shape hypothesis B1
    // predicts (FAKE; never an observation).
    CandidateReads GoodReads()
    {
        CandidateReads s_Reads;
        s_Reads.thread_id = GetCurrentThreadId();
        s_Reads.generation_fresh = Field<std::string>::Observed(k_Gen1);
        s_Reads.hero_entity_id = Field<std::string>::Observed(k_HeroId);
        s_Reads.hero_flags = Field<int64_t>::Observed(3);
        s_Reads.bricks.count = Field<int64_t>::Observed(24);
        s_Reads.bricks.truncated = Field<bool>::Observed(true);
        s_Reads.bricks.scanned = Field<int64_t>::Observed(24);
        s_Reads.brick_id = k_HitmenBrick;
        s_Reads.entity_id = k_Agent48;
        s_Reads.spawn_id = k_Agent48Spawn;
        s_Reads.brick_present = PlayerObservation::CandidatePresence(true, s_Reads.bricks);
        s_Reads.lookup = GoodLookup(3, 1, false);
        s_Reads.hitman5 = Field<bool>::Observed(true);
        s_Reads.cand_flags = Field<int64_t>::Observed(3);
        s_Reads.id_match = Field<bool>::Observed(true);
        s_Reads.cand_brick_owner = Field<std::string>::Observed(k_HitmenBrick);
        SetSlots(s_Reads, SlotMatch::Local, SlotMatch::Empty, SlotMatch::Empty, SlotMatch::Empty);
        s_Reads.hero = FakeHero(-109.58812, -89.883003, -0.536);
        s_Reads.candidate = FakeHero(0, 0, -1.5393555);
        s_Reads.spawn = FakeSpawn(k_Agent48, true);
        return s_Reads;
    }

    // The candidate brick absent: the reader's own marking (the same reasons the DLL applies).
    CandidateReads AbsentReads(size_t p_BrickCount)
    {
        CandidateReads s_Reads = GoodReads();
        s_Reads.bricks.count = Field<int64_t>::Observed(static_cast<int64_t>(p_BrickCount));
        s_Reads.bricks.scanned = Field<int64_t>::Observed(static_cast<int64_t>(std::min(p_BrickCount, PlayerObservation::k_MaxBricksScanned)));
        s_Reads.brick_present = PlayerObservation::CandidatePresence(false, s_Reads.bricks);
        const std::string s_Why = s_Reads.brick_present.observed() ? "brick not loaded" : "brick scan incomplete";
        PlayerObservation::MarkLookup(s_Reads.lookup, FieldStatus::Unavailable, s_Why.c_str());
        s_Reads.hitman5 = Field<bool>::Of(FieldStatus::Unavailable, s_Why);
        s_Reads.cand_flags = Field<int64_t>::Of(FieldStatus::Unavailable, s_Why);
        s_Reads.id_match = Field<bool>::Of(FieldStatus::Unavailable, s_Why);
        s_Reads.cand_brick_owner = Field<std::string>::Of(FieldStatus::Unavailable, s_Why);
        CandidateObservation::MarkHero(s_Reads.candidate, FieldStatus::Unavailable, s_Why.c_str());
        CandidateObservation::MarkSpawn(s_Reads.spawn, FieldStatus::Unavailable, s_Why.c_str());
        return s_Reads;
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
        return s_Value && s_Value->field_status && *s_Value->field_status == FieldStatus::Observed && s_Value->kind == ObservedValue::Kind::Text ? s_Value->text : std::string();
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

    size_t Index(const Result& p_Result, const char* p_Key)
    {
        for (size_t i = 0; i < p_Result.observed.size(); ++i)
            if (p_Result.observed[i].first == p_Key)
                return i;
        return SIZE_MAX;
    }

    bool Contains(const std::string& p_Text, const char* p_Needle)
    {
        return p_Text.find(p_Needle) != std::string::npos;
    }

    Result RunCandidate(const CommandScheduler::FrameObservation& p_Frame, CommandScheduler::CandidateInspector p_Inspector, const Command& p_Command, CommandScheduler::Stats* p_Stats = nullptr)
    {
        CommandQueue s_Queue;
        std::vector<Result> s_Results;
        CommandScheduler s_Scheduler(s_Queue, [&](const Result& p_Result) { s_Results.push_back(p_Result); });
        s_Queue.Push(p_Command, 1, p_Frame.now_mono_ms > 10 ? p_Frame.now_mono_ms - 10 : 0);
        CommandScheduler::FrameObservation s_Frame = p_Frame;
        s_Frame.candidate_inspector = std::move(p_Inspector);
        CHECK(s_Scheduler.Tick(s_Frame));
        CHECK(s_Results.size() == 1);
        if (p_Stats)
            *p_Stats = s_Scheduler.GetStats();
        return s_Results.empty() ? Result{} : s_Results[0];
    }

    Result RunWith(const CandidateReads& p_Reads, bool p_Playing = true, std::optional<std::string> p_Generation = k_Gen1, bool p_Available = true)
    {
        return RunCandidate(Frame(31000, 50000, p_Playing, p_Generation, p_Available), [&](const CandidateQuery&) { return p_Reads; }, CandidateCommand(CommandTest::Uuid(1)));
    }

    // The 47-entry read-back with the frame values first, the identity block, the registry verdict,
    // the candidate, the spawn, and the local hero last.
    const char* k_ExpectedOrder[] = {
        "mission_playing", "scene_type", "receipt_to_frame_ms",
        "generation_fresh", "hero_entity_id", "hero_flags", "brick_count", "bricks_scanned", "targets",
        "cand_brick_present", "cand_subentity_count", "cand_index", "cand_index_check", "cand_resolved", "cand_hitman5", "cand_flags", "cand_id_match", "cand_distinct", "cand_brick_owner",
        "registry_slots",
        "cand_world", "cand_local", "cand_parent", "cand_spatial_flags", "cand_members", "cand_controllers", "cand_outfit", "cand_active",
        "spawn_index", "spawn_index_check", "spawn_resolved", "spawn_class", "spawn_world", "spawn_dirty", "spawn_player_id", "spawn_select", "spawn_hitman_ref", "spawn_ref_is_cand",
        "hero_world", "hero_local", "hero_parent", "hero_spatial_flags", "hero_members", "hero_controllers", "hero_outfit", "hero_active",
        "reads_on_frame",
    };
    constexpr size_t k_ExpectedEntries = sizeof(k_ExpectedOrder) / sizeof(k_ExpectedOrder[0]);
}

void RunCandidateInspectionTests()
{
    // ---- the grammar -----------------------------------------------------------------------------
    {
        CHECK(std::string(CandidateObservation::MemberText(Member::Null)) == "0" && std::string(CandidateObservation::MemberText(Member::Present)) == "1" && std::string(CandidateObservation::MemberText(Member::Valid)) == "2");
        CHECK(std::string(CandidateObservation::SlotMatchText(SlotMatch::Empty)) == "empty" && std::string(CandidateObservation::SlotMatchText(SlotMatch::Inconclusive)) == "inconclusive");
        CHECK(CandidateObservation::Number(0) == "0" && CandidateObservation::Number(-1.5393555) == "-1.539356" && CandidateObservation::Number(-109.58812) == "-109.5881" && CandidateObservation::Number(0.4925565) == "0.4925565");
        CHECK(CandidateObservation::Number(1e10) == "1e+10" && CandidateObservation::Number(-123456789.0).size() <= 13);
        Vec3 s_V;
        s_V.x = -40.99882;
        s_V.y = -24.293657;
        s_V.z = 0.4925565;
        CHECK(CandidateObservation::EncodeVec3(s_V) == "-40.99882,-24.29366,0.4925565");
        LocalTransform s_L;
        s_L.translation = s_V;
        s_L.rotation.w = 1;
        CHECK(CandidateObservation::EncodeLocal(s_L) == "-40.99882,-24.29366,0.4925565|0,0,0,1");
        SpatialFlags s_F;
        s_F.visible = true;
        s_F.dynamic = true;
        s_F.dirty = true;
        s_F.room_behaviour = 1;
        s_F.transform_parent = true;
        CHECK(CandidateObservation::EncodeSpatialFlags(s_F) == "visible=1,parent_hidden=0,force_visible=0,dynamic=1,scene_child=0,dirty=1,room=1,tparent=1,tchildren=0");
        SpatialFlags s_Hidden;
        s_Hidden.parent_hidden = true;
        s_Hidden.force_visible = true;
        s_Hidden.scene_child = true;
        s_Hidden.transform_children = true;
        s_Hidden.room_behaviour = 2;
        CHECK(CandidateObservation::EncodeSpatialFlags(s_Hidden) == "visible=0,parent_hidden=1,force_visible=1,dynamic=0,scene_child=1,dirty=0,room=2,tparent=0,tchildren=1");
        HeroMembers s_M;
        s_M.character = Member::Valid;
        s_M.animator = Member::Present;
        s_M.health = Member::Present;
        s_M.outfit_kit = Member::Valid;
        CHECK(CandidateObservation::EncodeMembers(s_M) == "character=2,animator=1,morpheme=0,ragdoll=0,health=1,kit=2,camera=0,input=0");
        HeroControllers s_C;
        s_C.ik = Member::Valid;
        s_C.actor_tag = Member::Present;
        CHECK(CandidateObservation::EncodeControllers(s_C) == "guide=0,ik=2,cheat=0,interaction=0,actor_tag=1");
        CandidateObservation::OutfitTuple s_O;
        s_O.kit_id = k_Suit;
        s_O.charset = 0;
        s_O.variation = 3;
        CHECK(CandidateObservation::EncodeOutfit(s_O) == std::string(k_Suit) + ",0,3");
        CandidateObservation::OutfitTuple s_NoKit;
        s_NoKit.charset = 1;
        CHECK(CandidateObservation::EncodeOutfit(s_NoKit) == "none,1,0");
        CHECK(CandidateObservation::EncodeSelect(false, true) == "default=0,preferred=1");
        CHECK(CandidateObservation::EncodeTargets(k_HitmenBrick, k_Agent48, k_Agent48Spawn) == "007c57a6ea581f8c,feede715906f747f,feed755acdd56780");

        SlotIdentity s_Slots[4];
        s_Slots[0].match = SlotMatch::Local;
        s_Slots[0].hero_resolved = true;
        s_Slots[1].match = SlotMatch::Candidate;
        s_Slots[1].hero_resolved = false;
        s_Slots[2].match = SlotMatch::Other;
        s_Slots[2].hero_resolved = true;
        s_Slots[3].match = SlotMatch::Empty;
        CHECK(CandidateObservation::EncodeSlots(s_Slots) == "0:local,1:candidate-nohero,2:other,3:empty");

        // every packed text stays inside the 128-byte text bound at its longest
        SpatialFlags s_Max;
        s_Max.visible = s_Max.parent_hidden = s_Max.force_visible = s_Max.dynamic = s_Max.scene_child = s_Max.dirty = s_Max.transform_parent = s_Max.transform_children = true;
        s_Max.room_behaviour = 3;
        CHECK(CandidateObservation::EncodeSpatialFlags(s_Max).size() <= k_MaxObservedTextBytes);
        LocalTransform s_LongLocal;
        s_LongLocal.translation.x = s_LongLocal.translation.y = s_LongLocal.translation.z = -123456789.0;
        s_LongLocal.rotation.x = s_LongLocal.rotation.y = s_LongLocal.rotation.z = s_LongLocal.rotation.w = -0.70710678;
        CHECK(CandidateObservation::EncodeLocal(s_LongLocal).size() <= k_MaxObservedTextBytes);
        SlotIdentity s_LongSlots[4];
        for (auto& s_Slot : s_LongSlots)
            s_Slot.match = SlotMatch::Inconclusive;
        CHECK(CandidateObservation::EncodeSlots(s_LongSlots).size() <= k_MaxObservedTextBytes);
        CandidateObservation::OutfitTuple s_LongOutfit;
        s_LongOutfit.kit_id = k_Suit;
        s_LongOutfit.charset = 15;
        s_LongOutfit.variation = 63;
        CHECK(CandidateObservation::EncodeOutfit(s_LongOutfit).size() <= k_MaxObservedTextBytes);
    }

    // ---- the knowledge and slot rules ------------------------------------------------------------
    {
        CHECK(CandidateObservation::KnowledgeOf(true, Field<bool>::Observed(true)) == CandidateKnowledge::Resolved);
        CHECK(CandidateObservation::KnowledgeOf(false, Field<bool>::Observed(false)) == CandidateKnowledge::KnownAbsent);
        CHECK(CandidateObservation::KnowledgeOf(false, Field<bool>::Observed(true)) == CandidateKnowledge::Inconclusive);   // present but unresolved
        CHECK(CandidateObservation::KnowledgeOf(false, Field<bool>::Of(FieldStatus::Unavailable, "scan bound: 64/70 bricks scanned")) == CandidateKnowledge::Inconclusive);

        CHECK(CandidateObservation::ClassifySlot(false, false, false, CandidateKnowledge::Resolved) == SlotMatch::Empty);
        CHECK(CandidateObservation::ClassifySlot(false, true, true, CandidateKnowledge::Resolved) == SlotMatch::Empty);
        CHECK(CandidateObservation::ClassifySlot(true, true, false, CandidateKnowledge::Resolved) == SlotMatch::Local);
        CHECK(CandidateObservation::ClassifySlot(true, true, true, CandidateKnowledge::Resolved) == SlotMatch::Local);   // local first: the identity block reports "not distinct"
        CHECK(CandidateObservation::ClassifySlot(true, false, true, CandidateKnowledge::Resolved) == SlotMatch::Candidate);
        CHECK(CandidateObservation::ClassifySlot(true, false, false, CandidateKnowledge::Resolved) == SlotMatch::Other);
        CHECK(CandidateObservation::ClassifySlot(true, false, false, CandidateKnowledge::KnownAbsent) == SlotMatch::Other);
        CHECK(CandidateObservation::ClassifySlot(true, false, true, CandidateKnowledge::KnownAbsent) == SlotMatch::Other);   // no identity to match: a stale comparison cannot promote
        CHECK(CandidateObservation::ClassifySlot(true, false, false, CandidateKnowledge::Inconclusive) == SlotMatch::Inconclusive);
        CHECK(CandidateObservation::ClassifySlot(true, true, false, CandidateKnowledge::Inconclusive) == SlotMatch::Local);
    }

    // ---- the encoding of a full read set: order, count, values ----------------------------------
    {
        const Result s_Result = RunWith(GoodReads());
        CHECK(s_Result.status == Status::Applied && s_Result.reason.empty());
        CHECK(s_Result.observed.size() == k_ExpectedEntries && k_ExpectedEntries == 47);
        for (size_t i = 0; i < k_ExpectedEntries && i < s_Result.observed.size(); ++i)
            CHECK(s_Result.observed[i].first == k_ExpectedOrder[i]);
        CHECK(Index(s_Result, "registry_slots") < Index(s_Result, "cand_world") && Index(s_Result, "spawn_ref_is_cand") < Index(s_Result, "hero_world"));
        CHECK(TextField(s_Result, "generation_fresh") == k_Gen1 && TextField(s_Result, "hero_entity_id") == k_HeroId && IntField(s_Result, "hero_flags") == 3);
        CHECK(IntField(s_Result, "brick_count") == 24 && IntField(s_Result, "bricks_scanned") == 24);
        CHECK(TextField(s_Result, "targets") == "007c57a6ea581f8c,feede715906f747f,feed755acdd56780");
        CHECK(BoolField(s_Result, "cand_brick_present", true) && IntField(s_Result, "cand_subentity_count") == 3 && IntField(s_Result, "cand_index") == 1 && TextField(s_Result, "cand_index_check") == "in_range");
        CHECK(BoolField(s_Result, "cand_resolved", true) && BoolField(s_Result, "cand_hitman5", true) && IntField(s_Result, "cand_flags") == 3 && BoolField(s_Result, "cand_id_match", true));
        CHECK(BoolField(s_Result, "cand_distinct", true) && TextField(s_Result, "cand_brick_owner") == k_HitmenBrick);
        CHECK(TextField(s_Result, "registry_slots") == "0:local,1:empty,2:empty,3:empty");
        CHECK(TextField(s_Result, "cand_world") == "0,0,-1.539356" && TextField(s_Result, "hero_world") == "-109.5881,-89.883,-0.536");
        CHECK(TextField(s_Result, "cand_local") == "0,0,0|0,0,0,1" && TextField(s_Result, "cand_parent") == k_SceneRootId);
        CHECK(TextField(s_Result, "cand_spatial_flags") == "visible=1,parent_hidden=0,force_visible=0,dynamic=1,scene_child=0,dirty=0,room=1,tparent=1,tchildren=1");
        CHECK(TextField(s_Result, "cand_members") == "character=2,animator=1,morpheme=1,ragdoll=2,health=1,kit=2,camera=2,input=1");
        CHECK(TextField(s_Result, "cand_controllers") == "guide=2,ik=2,cheat=2,interaction=2,actor_tag=2");
        CHECK(TextField(s_Result, "cand_outfit") == std::string(k_Suit) + ",0,0" && BoolField(s_Result, "cand_active", true));
        CHECK(IntField(s_Result, "spawn_index") == 2 && TextField(s_Result, "spawn_index_check") == "in_range" && BoolField(s_Result, "spawn_resolved", true));
        CHECK(TextField(s_Result, "spawn_class") == "ZHeroSpawn" && TextField(s_Result, "spawn_world") == "-40.99882,-24.29366,0.4925565" && BoolField(s_Result, "spawn_dirty", false));
        CHECK(IntField(s_Result, "spawn_player_id") == 2 && TextField(s_Result, "spawn_select") == "default=0,preferred=0");
        CHECK(TextField(s_Result, "spawn_hitman_ref") == k_Agent48 && BoolField(s_Result, "spawn_ref_is_cand", true));
        CHECK(BoolField(s_Result, "hero_active", true) && BoolField(s_Result, "reads_on_frame", true));
        CHECK(!Observed(s_Result, "reasons_dropped") && !Observed(s_Result, "observed_reduced"));
        // no setter, no transform, no input, no registry field exists in the read-back
        for (const auto& s_Entry : s_Result.observed)
            CHECK(!Contains(s_Entry.first, "set") || s_Entry.first == "spawn_select");
        const std::string s_Line = EncodeResult(s_Result);
        std::printf("line bound: candidate good read set = %zu bytes of %zu (47 entries)\n", s_Line.size(), k_MaxLineBytes);
        CHECK(s_Line.size() <= k_MaxLineBytes && CommandScheduler::WithinLineBound(s_Result));
    }

    // ---- no inspector: every engine field unsupported, nothing guessed --------------------------
    // (43 entries with the shared 33-byte "no inspector" reason exceed the line bound, so the
    // scheduler's first reduction stage drops the reasons: every field keeps its status, the marker
    // and the note say so; this frame source exists only in the tests and the probe's wiring)
    {
        CommandScheduler::Stats s_Stats;
        const Result s_Result = RunCandidate(Frame(5, 1000, false, std::nullopt), nullptr, CandidateCommand(CommandTest::Uuid(2)), &s_Stats);
        CHECK(s_Result.status == Status::Applied && Contains(s_Result.reason, "engine reads unsupported on this frame source") && Contains(s_Result.reason, "field reasons dropped"));
        CHECK(s_Result.observed.size() == k_ExpectedEntries + 1 && Observed(s_Result, "reasons_dropped") && !Observed(s_Result, "observed_reduced"));
        CHECK(FieldIs(s_Result, "generation_fresh", FieldStatus::Unsupported) && FieldIs(s_Result, "cand_world", FieldStatus::Unsupported) && FieldIs(s_Result, "spawn_player_id", FieldStatus::Unsupported) && FieldIs(s_Result, "registry_slots", FieldStatus::Unsupported) && FieldIs(s_Result, "hero_active", FieldStatus::Unsupported));
        CHECK(Reason(s_Result, "cand_members").empty() && FieldIs(s_Result, "reads_on_frame", FieldStatus::Unsupported));
        CHECK(CommandScheduler::WithinLineBound(s_Result) && s_Stats.reduced == 1 && s_Stats.unbounded == 0);
        for (size_t i = 0; i < k_ExpectedEntries && i < s_Result.observed.size(); ++i)
            CHECK(s_Result.observed[i].first == k_ExpectedOrder[i]); // the order survives the reduction
        CHECK(TextField(s_Result, "targets") == "007c57a6ea581f8c,feede715906f747f,feed755acdd56780"); // the historical defaults echoed
        CHECK(BoolField(s_Result, "mission_playing", false) && FieldIs(s_Result, "scene_type", FieldStatus::Observed));
        CHECK(s_Stats.candidates_inspected == 1 && s_Stats.entities_inspected == 0 && s_Stats.players_inspected == 0 && s_Stats.outfit_called == 0 && s_Stats.outfit_dispatched == 0);
        for (const auto& s_Entry : s_Result.observed)
            if (s_Entry.first.rfind("cand_", 0) == 0 || s_Entry.first.rfind("spawn_", 0) == 0)
                CHECK(s_Entry.second.kind == ObservedValue::Kind::Null);

        // the explicit ids travel to the inspector and are echoed back
        CandidateQuery s_Seen;
        const Result s_Named = RunCandidate(Frame(6, 1000, true), [&](const CandidateQuery& p_Query) { s_Seen = p_Query; return GoodReads(); }, CandidateCommand(CommandTest::Uuid(3), std::string(k_HitmenBrick), std::string(k_Agent48)));
        CHECK(s_Seen.brick && *s_Seen.brick == k_HitmenBrick && s_Seen.entity_id && *s_Seen.entity_id == k_Agent48 && s_Named.status == Status::Applied);
    }

    // ---- the candidate brick absent: complete scan (known absent) and incomplete scan ------------
    {
        const Result s_Absent = RunWith(AbsentReads(23));
        CHECK(s_Absent.status == Status::Applied && BoolField(s_Absent, "cand_brick_present", false));
        CHECK(FieldIs(s_Absent, "cand_index", FieldStatus::Unavailable) && Reason(s_Absent, "cand_index") == "brick not loaded");
        CHECK(FieldIs(s_Absent, "cand_resolved", FieldStatus::Unavailable) && FieldIs(s_Absent, "cand_hitman5", FieldStatus::Unavailable) && FieldIs(s_Absent, "cand_distinct", FieldStatus::Unavailable));
        CHECK(FieldIs(s_Absent, "cand_world", FieldStatus::Unavailable) && FieldIs(s_Absent, "cand_active", FieldStatus::Unavailable) && FieldIs(s_Absent, "spawn_resolved", FieldStatus::Unavailable) && FieldIs(s_Absent, "spawn_player_id", FieldStatus::Unavailable));
        CHECK(FieldIs(s_Absent, "hero_world", FieldStatus::Observed) && BoolField(s_Absent, "hero_active", true) && TextField(s_Absent, "registry_slots") == "0:local,1:empty,2:empty,3:empty");
        CHECK(s_Absent.observed.size() == k_ExpectedEntries && !Observed(s_Absent, "reasons_dropped"));
        const std::string s_Line = EncodeResult(s_Absent);
        std::printf("line bound: candidate absent (complete scan) = %zu bytes of %zu\n", s_Line.size(), k_MaxLineBytes);
        CHECK(s_Line.size() <= k_MaxLineBytes);

        const Result s_Partial = RunWith(AbsentReads(70));
        CHECK(FieldIs(s_Partial, "cand_brick_present", FieldStatus::Unavailable) && Reason(s_Partial, "cand_brick_present") == "scan bound: 64/70 bricks scanned");
        CHECK(IntField(s_Partial, "brick_count") == 70 && IntField(s_Partial, "bricks_scanned") == 64 && Reason(s_Partial, "cand_index") == "brick scan incomplete");
        CHECK(!Observed(s_Partial, "reasons_dropped") && s_Partial.observed.size() == k_ExpectedEntries);
        const std::string s_PartialLine = EncodeResult(s_Partial);
        std::printf("line bound: candidate absence not established (70 bricks) = %zu bytes of %zu\n", s_PartialLine.size(), k_MaxLineBytes);
        CHECK(s_PartialLine.size() <= k_MaxLineBytes);

        // a fault inside the reads (the plugin's guard): every field unavailable with the fault
        // reason, the targets echoed, and the line still whole (no reduction)
        CandidateQuery s_Query;
        CandidateReads s_Faulted = CandidateObservation::AllCandidate(FieldStatus::Unavailable, "candidate reads faulted", s_Query);
        s_Faulted.thread_id = GetCurrentThreadId();
        const Result s_Fault = RunWith(s_Faulted);
        CHECK(s_Fault.status == Status::Applied && FieldIs(s_Fault, "hero_entity_id", FieldStatus::Unavailable) && FieldIs(s_Fault, "registry_slots", FieldStatus::Unavailable) && Reason(s_Fault, "spawn_class") == "candidate reads faulted");
        CHECK(!Observed(s_Fault, "reasons_dropped") && s_Fault.observed.size() == k_ExpectedEntries && TextField(s_Fault, "targets") == "007c57a6ea581f8c,feede715906f747f,feed755acdd56780");
        const std::string s_FaultLine = EncodeResult(s_Fault);
        std::printf("line bound: candidate reads faulted (all unavailable) = %zu bytes of %zu\n", s_FaultLine.size(), k_MaxLineBytes);
        CHECK(s_FaultLine.size() <= k_MaxLineBytes);
    }

    // ---- the candidate entity missing, the hero interface missing, the spawn missing ----------
    {
        CandidateReads s_Reads = GoodReads();
        s_Reads.lookup = LookupWith(3, -1); // not a sub-entity of the brick's factory
        s_Reads.hitman5 = Field<bool>::Of(FieldStatus::Unavailable, "candidate entity not resolved");
        s_Reads.cand_flags = Field<int64_t>::Of(FieldStatus::Unavailable, "candidate entity not resolved");
        s_Reads.id_match = Field<bool>::Of(FieldStatus::Unavailable, "candidate entity not resolved");
        s_Reads.cand_brick_owner = Field<std::string>::Of(FieldStatus::Unavailable, "candidate entity not resolved");
        CandidateObservation::MarkHero(s_Reads.candidate, FieldStatus::Unavailable, "candidate entity not resolved");
        s_Reads.spawn.ref_is_candidate = Field<bool>::Of(FieldStatus::Unavailable, "candidate entity not resolved");
        SetSlots(s_Reads, SlotMatch::Local, SlotMatch::Empty, SlotMatch::Empty, SlotMatch::Empty);
        const Result s_Missing = RunWith(s_Reads);
        CHECK(BoolField(s_Missing, "cand_brick_present", true) && IntField(s_Missing, "cand_index") == -1 && TextField(s_Missing, "cand_index_check") == "negative");
        CHECK(FieldIs(s_Missing, "cand_resolved", FieldStatus::Unavailable) && FieldIs(s_Missing, "cand_members", FieldStatus::Unavailable) && Reason(s_Missing, "cand_members") == "candidate entity not resolved");
        CHECK(BoolField(s_Missing, "spawn_resolved", true) && TextField(s_Missing, "spawn_hitman_ref") == k_Agent48 && FieldIs(s_Missing, "spawn_ref_is_cand", FieldStatus::Unavailable));

        CandidateReads s_NoHero = GoodReads();
        s_NoHero.hitman5 = Field<bool>::Observed(false);
        s_NoHero.candidate.members = Field<HeroMembers>::Of(FieldStatus::Unavailable, "hero interface not resolved");
        s_NoHero.candidate.controllers = Field<HeroControllers>::Of(FieldStatus::Unavailable, "hero interface not resolved");
        s_NoHero.candidate.outfit = Field<CandidateObservation::OutfitTuple>::Of(FieldStatus::Unavailable, "hero interface not resolved");
        s_NoHero.candidate.active = Field<bool>::Of(FieldStatus::Unavailable, "hero interface not resolved");
        const Result s_Interface = RunWith(s_NoHero);
        CHECK(BoolField(s_Interface, "cand_resolved", true) && BoolField(s_Interface, "cand_hitman5", false));
        CHECK(FieldIs(s_Interface, "cand_members", FieldStatus::Unavailable) && FieldIs(s_Interface, "cand_active", FieldStatus::Unavailable) && FieldIs(s_Interface, "cand_world", FieldStatus::Observed));
        // missing components are unavailable, never absent by assumption: no "character=0" was invented
        CHECK(TextField(s_Interface, "cand_members").empty());

        CandidateReads s_NoSpawn = GoodReads();
        const PlayerObservation::LookupFacts s_SpawnLookup = LookupWith(3, -1);
        CandidateObservation::MarkSpawn(s_NoSpawn.spawn, FieldStatus::Unavailable, "spawn entity not resolved");
        s_NoSpawn.spawn.lookup = s_SpawnLookup;
        const Result s_Spawnless = RunWith(s_NoSpawn);
        CHECK(IntField(s_Spawnless, "spawn_index") == -1 && TextField(s_Spawnless, "spawn_index_check") == "negative" && FieldIs(s_Spawnless, "spawn_resolved", FieldStatus::Unavailable));
        CHECK(FieldIs(s_Spawnless, "spawn_class", FieldStatus::Unavailable) && Reason(s_Spawnless, "spawn_player_id") == "spawn entity not resolved" && FieldIs(s_Spawnless, "spawn_select", FieldStatus::Unavailable));
        CHECK(BoolField(s_Spawnless, "cand_resolved", true) && BoolField(s_Spawnless, "cand_hitman5", true)); // the candidate's own facts untouched
    }

    // ---- the spawn's hero reference: the candidate, a different entity, none ------------------
    {
        const Result s_Self = RunWith(GoodReads());
        CHECK(TextField(s_Self, "spawn_hitman_ref") == k_Agent48 && BoolField(s_Self, "spawn_ref_is_cand", true));

        CandidateReads s_Other = GoodReads();
        s_Other.spawn = FakeSpawn(k_OtherId, false);
        const Result s_Different = RunWith(s_Other);
        CHECK(TextField(s_Different, "spawn_hitman_ref") == k_OtherId && BoolField(s_Different, "spawn_ref_is_cand", false));

        CandidateReads s_None = GoodReads();
        s_None.spawn = FakeSpawn("none", false);
        const Result s_Unset = RunWith(s_None);
        CHECK(TextField(s_Unset, "spawn_hitman_ref") == "none" && BoolField(s_Unset, "spawn_ref_is_cand", false));

        CandidateReads s_Unreadable = GoodReads();
        s_Unreadable.spawn.hitman_ref = Field<std::string>::Of(FieldStatus::Unsupported, "property has an engine getter");
        s_Unreadable.spawn.ref_is_candidate = Field<bool>::Of(FieldStatus::Unsupported, "property has an engine getter");
        s_Unreadable.spawn.player_id = Field<int64_t>::Of(FieldStatus::Unsupported, "property type int64");
        const Result s_Prop = RunWith(s_Unreadable);
        CHECK(FieldIs(s_Prop, "spawn_hitman_ref", FieldStatus::Unsupported) && FieldIs(s_Prop, "spawn_ref_is_cand", FieldStatus::Unsupported) && FieldIs(s_Prop, "spawn_player_id", FieldStatus::Unsupported));
        CHECK(TextField(s_Prop, "spawn_select") == "default=0,preferred=0"); // the other properties keep their own statuses
    }

    // ---- the factory answers: an invalid count, a negative index, an index equal to the count -----
    {
        CandidateReads s_Invalid = GoodReads();
        s_Invalid.lookup = LookupWith(-1, 0);
        CandidateObservation::MarkHero(s_Invalid.candidate, FieldStatus::Unavailable, "candidate entity not resolved");
        const Result s_CountInvalid = RunWith(s_Invalid);
        CHECK(IntField(s_CountInvalid, "cand_subentity_count") == -1 && FieldIs(s_CountInvalid, "cand_index", FieldStatus::Unavailable) && TextField(s_CountInvalid, "cand_index_check") == "count_invalid");
        CHECK(FieldIs(s_CountInvalid, "cand_resolved", FieldStatus::Inconsistent) && FieldIs(s_CountInvalid, "cand_distinct", FieldStatus::Inconsistent));

        CandidateReads s_Negative = GoodReads();
        s_Negative.lookup = LookupWith(3, -1);
        const Result s_NegativeResult = RunWith(s_Negative);
        CHECK(TextField(s_NegativeResult, "cand_index_check") == "negative" && FieldIs(s_NegativeResult, "cand_resolved", FieldStatus::Unavailable) && Reason(s_NegativeResult, "cand_resolved") == "not a sub-entity of that factory");

        CandidateReads s_AtCount = GoodReads();
        s_AtCount.lookup = LookupWith(3, 3);
        const Result s_AtCountResult = RunWith(s_AtCount);
        CHECK(IntField(s_AtCountResult, "cand_index") == 3 && TextField(s_AtCountResult, "cand_index_check") == "out_of_range" && FieldIs(s_AtCountResult, "cand_resolved", FieldStatus::Inconsistent));
        CHECK(TextField(s_AtCountResult, "cand_index_check") != "negative");

        // the same rule for the spawn's lookup
        CandidateReads s_SpawnAtCount = GoodReads();
        s_SpawnAtCount.spawn.lookup = LookupWith(3, 3);
        const Result s_SpawnResult = RunWith(s_SpawnAtCount);
        CHECK(IntField(s_SpawnResult, "spawn_index") == 3 && TextField(s_SpawnResult, "spawn_index_check") == "out_of_range" && FieldIs(s_SpawnResult, "spawn_resolved", FieldStatus::Inconsistent));
    }

    // ---- the parent: absent, unavailable; dirty transform; visible and hidden -------------------
    {
        CandidateReads s_NoParent = GoodReads();
        s_NoParent.candidate.spatial.parent = Field<std::string>::Observed("none");
        const Result s_Orphan = RunWith(s_NoParent);
        CHECK(TextField(s_Orphan, "cand_parent") == "none" && TextField(s_Orphan, "hero_parent") == k_SceneRootId);

        CandidateReads s_BadParent = GoodReads();
        s_BadParent.candidate.spatial.parent = Field<std::string>::Of(FieldStatus::Unavailable, "entity type null");
        const Result s_Unavailable = RunWith(s_BadParent);
        CHECK(FieldIs(s_Unavailable, "cand_parent", FieldStatus::Unavailable) && Reason(s_Unavailable, "cand_parent") == "entity type null" && FieldIs(s_Unavailable, "cand_world", FieldStatus::Observed));

        CandidateReads s_Dirty = GoodReads();
        SpatialFlags s_Flags = *s_Dirty.candidate.spatial.flags.value;
        s_Flags.dirty = true;
        s_Dirty.candidate.spatial.flags = Field<SpatialFlags>::Observed(s_Flags);
        const Result s_DirtyResult = RunWith(s_Dirty);
        CHECK(Contains(TextField(s_DirtyResult, "cand_spatial_flags"), ",dirty=1,") && Contains(TextField(s_DirtyResult, "hero_spatial_flags"), ",dirty=0,"));
        CHECK(TextField(s_DirtyResult, "cand_world") == "0,0,-1.539356"); // the stored value is still reported; the flag labels it potentially stale

        CandidateReads s_Hidden = GoodReads();
        SpatialFlags s_HiddenFlags = *s_Hidden.candidate.spatial.flags.value;
        s_HiddenFlags.visible = false;
        s_HiddenFlags.parent_hidden = true;
        s_Hidden.candidate.spatial.flags = Field<SpatialFlags>::Observed(s_HiddenFlags);
        const Result s_HiddenResult = RunWith(s_Hidden);
        CHECK(Contains(TextField(s_HiddenResult, "cand_spatial_flags"), "visible=0,parent_hidden=1,") && Contains(TextField(s_HiddenResult, "hero_spatial_flags"), "visible=1,parent_hidden=0,"));

        CandidateReads s_NoSpatial = GoodReads();
        CandidateObservation::MarkSpatial(s_NoSpatial.candidate.spatial, FieldStatus::Unavailable, "spatial interface not resolved");
        const Result s_NoSpatialResult = RunWith(s_NoSpatial);
        CHECK(FieldIs(s_NoSpatialResult, "cand_world", FieldStatus::Unavailable) && FieldIs(s_NoSpatialResult, "cand_local", FieldStatus::Unavailable) && FieldIs(s_NoSpatialResult, "cand_spatial_flags", FieldStatus::Unavailable) && FieldIs(s_NoSpatialResult, "cand_members", FieldStatus::Observed));
    }

    // ---- the members: all present, partially missing; m_bActive unavailable, true, false ------
    {
        const Result s_Complete = RunWith(GoodReads());
        CHECK(TextField(s_Complete, "cand_members") == "character=2,animator=1,morpheme=1,ragdoll=2,health=1,kit=2,camera=2,input=1");

        CandidateReads s_Partial = GoodReads();
        s_Partial.candidate = FakeHero(0, 0, -1.5393555, false);
        const Result s_PartialResult = RunWith(s_Partial);
        CHECK(TextField(s_PartialResult, "cand_members") == "character=0,animator=1,morpheme=1,ragdoll=0,health=0,kit=2,camera=0,input=0");
        CHECK(TextField(s_PartialResult, "cand_controllers") == "guide=0,ik=1,cheat=0,interaction=0,actor_tag=0");
        CHECK(TextField(s_PartialResult, "hero_members") == "character=2,animator=1,morpheme=1,ragdoll=2,health=1,kit=2,camera=2,input=1");

        CandidateReads s_NoActive = GoodReads();
        s_NoActive.candidate.active = Field<bool>::Of(FieldStatus::Unavailable, "property not declared on this type");
        const Result s_NoActiveResult = RunWith(s_NoActive);
        CHECK(FieldIs(s_NoActiveResult, "cand_active", FieldStatus::Unavailable) && Reason(s_NoActiveResult, "cand_active") == "property not declared on this type" && BoolField(s_NoActiveResult, "hero_active", true));

        CandidateReads s_Getter = GoodReads();
        s_Getter.candidate.active = Field<bool>::Of(FieldStatus::Unsupported, "property has an engine getter");
        CHECK(FieldIs(RunWith(s_Getter), "cand_active", FieldStatus::Unsupported));

        CandidateReads s_ActiveFalse = GoodReads();
        s_ActiveFalse.candidate.active = Field<bool>::Observed(false);
        const Result s_False = RunWith(s_ActiveFalse);
        CHECK(BoolField(s_False, "cand_active", false) && BoolField(s_False, "hero_active", true));
        CandidateReads s_ActiveTrue = GoodReads();
        s_ActiveTrue.candidate.active = Field<bool>::Observed(true);
        CHECK(BoolField(RunWith(s_ActiveTrue), "cand_active", true));
    }

    // ---- registry membership by identity ----------------------------------------------------------
    {
        CandidateReads s_Local = GoodReads();
        SetSlots(s_Local, SlotMatch::Local, SlotMatch::Empty, SlotMatch::Empty, SlotMatch::Empty);
        CHECK(TextField(RunWith(s_Local), "registry_slots") == "0:local,1:empty,2:empty,3:empty");

        CandidateReads s_Slot1 = GoodReads();
        SetSlots(s_Slot1, SlotMatch::Local, SlotMatch::Candidate, SlotMatch::Empty, SlotMatch::Empty);
        CHECK(TextField(RunWith(s_Slot1), "registry_slots") == "0:local,1:candidate,2:empty,3:empty");

        CandidateReads s_Slot1NoHero = GoodReads();
        SetSlots(s_Slot1NoHero, SlotMatch::Local, SlotMatch::Candidate, SlotMatch::Empty, SlotMatch::Empty, false);
        CHECK(TextField(RunWith(s_Slot1NoHero), "registry_slots") == "0:local,1:candidate-nohero,2:empty,3:empty");

        CandidateReads s_NoSlot = GoodReads();
        SetSlots(s_NoSlot, SlotMatch::Local, SlotMatch::Other, SlotMatch::Empty, SlotMatch::Empty);
        CHECK(TextField(RunWith(s_NoSlot), "registry_slots") == "0:local,1:other,2:empty,3:empty");

        CandidateReads s_Inconclusive = AbsentReads(70);
        SetSlots(s_Inconclusive, SlotMatch::Local, SlotMatch::Inconclusive, SlotMatch::Empty, SlotMatch::Empty);
        CHECK(TextField(RunWith(s_Inconclusive), "registry_slots") == "0:local,1:inconclusive,2:empty,3:empty");

        CandidateReads s_NoRegistry = GoodReads();
        s_NoRegistry.registry_slots = Field<std::string>::Of(FieldStatus::Unavailable, "player registry global null");
        CHECK(FieldIs(RunWith(s_NoRegistry), "registry_slots", FieldStatus::Unavailable));

        // the player id 2 on the spawn never becomes a slot classification
        CandidateReads s_PlayerId = GoodReads();
        s_PlayerId.spawn.player_id = Field<int64_t>::Observed(2);
        SetSlots(s_PlayerId, SlotMatch::Local, SlotMatch::Empty, SlotMatch::Empty, SlotMatch::Empty);
        const Result s_NotMember = RunWith(s_PlayerId);
        CHECK(IntField(s_NotMember, "spawn_player_id") == 2 && TextField(s_NotMember, "registry_slots") == "0:local,1:empty,2:empty,3:empty");
    }

    // ---- a mission generation change and a scene unload -------------------------------------------
    {
        CandidateReads s_Gen1 = GoodReads();
        const Result s_First = RunWith(s_Gen1, true, k_Gen1);
        CandidateReads s_Gen2 = GoodReads();
        s_Gen2.generation_fresh = Field<std::string>::Observed(k_Gen2);
        const Result s_Second = RunWith(s_Gen2, true, k_Gen2);
        CHECK(s_First.mission_generation && *s_First.mission_generation == k_Gen1 && TextField(s_First, "generation_fresh") == k_Gen1);
        CHECK(s_Second.mission_generation && *s_Second.mission_generation == k_Gen2 && TextField(s_Second, "generation_fresh") == k_Gen2);
        CHECK(TextField(s_First, "generation_fresh") != TextField(s_Second, "generation_fresh"));

        // the registry ahead of the edge-cached generation: reported as read, never reconciled
        CandidateReads s_Ahead = GoodReads();
        s_Ahead.generation_fresh = Field<std::string>::Observed(k_Gen2);
        const Result s_AheadResult = RunWith(s_Ahead, true, k_Gen1);
        CHECK(*s_AheadResult.mission_generation == k_Gen1 && TextField(s_AheadResult, "generation_fresh") == k_Gen2);
        CandidateReads s_Torn = GoodReads();
        s_Torn.generation_fresh = Field<std::string>::Of(FieldStatus::Inconsistent, "registry session id reads differ");
        CHECK(FieldIs(RunWith(s_Torn), "generation_fresh", FieldStatus::Inconsistent));

        // the scene unloaded: the reader finds no scene context and marks the candidate block; the
        // frame values say the scene state is unavailable; the local hero may still resolve (menu)
        CandidateReads s_Unloaded = GoodReads();
        s_Unloaded.generation_fresh = Field<std::string>::Of(FieldStatus::Unavailable, "registry session id empty or implausible");
        s_Unloaded.bricks.count = Field<int64_t>::Of(FieldStatus::Unavailable, "scene context null");
        s_Unloaded.bricks.scanned = Field<int64_t>::Of(FieldStatus::Unavailable, "scene context null");
        s_Unloaded.brick_present = Field<bool>::Of(FieldStatus::Unavailable, "scene context null");
        PlayerObservation::MarkLookup(s_Unloaded.lookup, FieldStatus::Unavailable, "scene context null");
        s_Unloaded.hitman5 = Field<bool>::Of(FieldStatus::Unavailable, "scene context null");
        s_Unloaded.cand_flags = Field<int64_t>::Of(FieldStatus::Unavailable, "scene context null");
        s_Unloaded.id_match = Field<bool>::Of(FieldStatus::Unavailable, "scene context null");
        s_Unloaded.cand_brick_owner = Field<std::string>::Of(FieldStatus::Unavailable, "scene context null");
        CandidateObservation::MarkHero(s_Unloaded.candidate, FieldStatus::Unavailable, "scene context null");
        CandidateObservation::MarkSpawn(s_Unloaded.spawn, FieldStatus::Unavailable, "scene context null");
        SetSlots(s_Unloaded, SlotMatch::Local, SlotMatch::Inconclusive, SlotMatch::Empty, SlotMatch::Empty);
        const Result s_UnloadResult = RunWith(s_Unloaded, false, std::nullopt, false);
        CHECK(s_UnloadResult.status == Status::Applied && !s_UnloadResult.mission_generation && BoolField(s_UnloadResult, "mission_playing", false));
        CHECK(FieldIs(s_UnloadResult, "scene_type", FieldStatus::Unavailable) && FieldIs(s_UnloadResult, "brick_count", FieldStatus::Unavailable) && Reason(s_UnloadResult, "cand_brick_present") == "scene context null");
        CHECK(FieldIs(s_UnloadResult, "cand_world", FieldStatus::Unavailable) && FieldIs(s_UnloadResult, "spawn_class", FieldStatus::Unavailable) && FieldIs(s_UnloadResult, "hero_world", FieldStatus::Observed));
        CHECK(TextField(s_UnloadResult, "registry_slots") == "0:local,1:inconclusive,2:empty,3:empty");
        const std::string s_Line = EncodeResult(s_UnloadResult);
        std::printf("line bound: candidate scene unloaded = %zu bytes of %zu\n", s_Line.size(), k_MaxLineBytes);
        CHECK(s_Line.size() <= k_MaxLineBytes);
    }

    // ---- the line bound: the maximal observed read set fits whole; the maximal reasons reduce -----
    {
        // every text at its grammar maximum, every number at its longest rendering
        CandidateReads s_Max = GoodReads();
        s_Max.generation_fresh = Field<std::string>::Observed(std::string(64, 'g'));
        s_Max.bricks.count = Field<int64_t>::Observed(1 << 20);
        s_Max.bricks.scanned = Field<int64_t>::Observed(64);
        s_Max.lookup = GoodLookup(1 << 20, (1 << 20) - 1, false);
        s_Max.hero_flags = Field<int64_t>::Observed(127);
        s_Max.cand_flags = Field<int64_t>::Observed(127);
        s_Max.cand_brick_owner = Field<std::string>::Observed(k_HitmenBrick);
        SlotIdentity s_Slots[4];
        for (auto& s_Slot : s_Slots)
            s_Slot.match = SlotMatch::Inconclusive;
        s_Max.registry_slots = Field<std::string>::Observed(CandidateObservation::EncodeSlots(s_Slots));
        auto s_MaxHero = [&]() {
            HeroReads s_Hero = FakeHero(-123456789.0, -123456789.0, -123456789.0);
            LocalTransform s_Local;
            s_Local.translation.x = s_Local.translation.y = s_Local.translation.z = -123456789.0;
            s_Local.rotation.x = s_Local.rotation.y = s_Local.rotation.z = s_Local.rotation.w = -0.70710678;
            s_Hero.spatial.local = Field<LocalTransform>::Observed(s_Local);
            SpatialFlags s_Flags;
            s_Flags.visible = s_Flags.parent_hidden = s_Flags.force_visible = s_Flags.dynamic = s_Flags.scene_child = s_Flags.dirty = s_Flags.transform_parent = s_Flags.transform_children = true;
            s_Flags.room_behaviour = 3;
            s_Hero.spatial.flags = Field<SpatialFlags>::Observed(s_Flags);
            CandidateObservation::OutfitTuple s_Outfit;
            s_Outfit.kit_id = k_Suit;
            s_Outfit.charset = 15;
            s_Outfit.variation = 63;
            s_Hero.outfit = Field<CandidateObservation::OutfitTuple>::Observed(s_Outfit);
            return s_Hero;
        };
        s_Max.hero = s_MaxHero();
        s_Max.candidate = s_MaxHero();
        s_Max.spawn = FakeSpawn(k_OtherId, false);
        s_Max.spawn.lookup = GoodLookup(1 << 20, (1 << 20) - 2, false);
        s_Max.spawn.class_name = Field<std::string>::Observed(std::string(CandidateObservation::k_MaxClassNameBytes, 'Z'));
        Vec3 s_Far;
        s_Far.x = s_Far.y = s_Far.z = -123456789.0;
        s_Max.spawn.world = Field<Vec3>::Observed(s_Far);
        s_Max.spawn.player_id = Field<int64_t>::Observed(-2147483648LL);
        CommandScheduler::FrameObservation s_Frame = Frame(18446744073709551615ull, 18446744073709551615ull, true, std::string(64, 'g'));
        s_Frame.adapter_instance_id = CommandTest::Uuid(99);
        s_Frame.native_at = std::string(64, 'n');
        Result s_Result = RunCandidate(s_Frame, [&](const CandidateQuery&) { return s_Max; }, CandidateCommand(CommandTest::Uuid(7)));
        const std::string s_Line = EncodeResult(s_Result);
        std::printf("line bound: candidate maximal observed read set = %zu bytes of %zu (no reduction expected)\n", s_Line.size(), k_MaxLineBytes);
        CHECK(s_Line.size() <= k_MaxLineBytes && s_Result.observed.size() == k_ExpectedEntries && !Observed(s_Result, "reasons_dropped") && !Observed(s_Result, "observed_reduced"));
        CHECK(s_Result.status == Status::Applied && s_Result.reason.empty());

        // every field unavailable with a maximal 40-byte reason: the line exceeds the bound with the
        // reasons and fits without them (stage 1), every field kept, the markers placed, the
        // identity and frame metadata untouched, the invariant re-checked on the actual encoding
        const std::string s_MaxReason(k_MaxObservedReasonBytes, 'r');
        CandidateQuery s_Query;
        CandidateReads s_AllUnavailable = CandidateObservation::AllCandidate(FieldStatus::Unavailable, s_MaxReason.c_str(), s_Query);
        s_AllUnavailable.thread_id = GetCurrentThreadId();
        Result s_Reduced = RunCandidate(s_Frame, [&](const CandidateQuery&) { return s_AllUnavailable; }, CandidateCommand(CommandTest::Uuid(8)));
        const std::string s_ReducedLine = EncodeResult(s_Reduced);
        std::printf("line bound: candidate all-unavailable maximal reasons = %zu bytes of %zu after the scheduler's reduction\n", s_ReducedLine.size(), k_MaxLineBytes);
        CHECK(s_ReducedLine.size() <= k_MaxLineBytes && CommandScheduler::WithinLineBound(s_Reduced));
        CHECK(s_Reduced.status == Status::Applied && Observed(s_Reduced, "reasons_dropped") && Contains(s_Reduced.reason, "field reasons dropped"));
        CHECK(!Observed(s_Reduced, "observed_reduced") && s_Reduced.observed.size() == k_ExpectedEntries + 1); // stage 1 only: every field kept, plus the marker
        CHECK(Reason(s_Reduced, "cand_world").empty() && FieldIs(s_Reduced, "cand_world", FieldStatus::Unavailable) && FieldIs(s_Reduced, "registry_slots", FieldStatus::Unavailable));
        CHECK(s_Reduced.command_id == CommandTest::Uuid(8) && s_Reduced.link_generation == 1 && s_Reduced.frame && *s_Reduced.frame == 18446744073709551615ull && s_Reduced.mission_generation && *s_Reduced.mission_generation == std::string(64, 'g'));
        CHECK(TextField(s_Reduced, "targets") == "007c57a6ea581f8c,feede715906f747f,feed755acdd56780");
        // the unreduced line, measured directly, really was over the bound
        Result s_Measured;
        s_Measured.command_id = s_Reduced.command_id;
        s_Measured.link_generation = 1;
        s_Measured.adapter_instance_id = s_Frame.adapter_instance_id;
        s_Measured.native_at = s_Frame.native_at;
        s_Measured.frame = s_Frame.frame;
        s_Measured.mission_generation = s_Frame.mission_generation;
        s_Measured.status = Status::Applied;
        OutfitObservation::Append(s_Measured.observed, "mission_playing", Field<bool>::Observed(true));
        OutfitObservation::Append(s_Measured.observed, "scene_type", Field<std::string>::Observed("mission"));
        OutfitObservation::Append(s_Measured.observed, "receipt_to_frame_ms", Field<int64_t>::Observed(10));
        CandidateObservation::AppendCandidate(s_Measured.observed, s_AllUnavailable);
        OutfitObservation::Append(s_Measured.observed, "reads_on_frame", Field<bool>::Observed(true));
        std::printf("line bound: candidate all-unavailable maximal reasons = %zu bytes before reduction\n", EncodeResult(s_Measured).size());
        CHECK(EncodeResult(s_Measured).size() > k_MaxLineBytes);
        CHECK(CommandScheduler::ReduceToFit(s_Measured) && CommandScheduler::WithinLineBound(s_Measured));
    }

    // ---- the stale-generation and TTL paths are the scheduler's own, unchanged -------------------
    {
        CommandQueue s_Queue;
        std::vector<Result> s_Results;
        CommandScheduler s_Scheduler(s_Queue, [&](const Result& p_Result) { s_Results.push_back(p_Result); });
        s_Queue.Push(CandidateCommand(CommandTest::Uuid(9)), 1, 1000);
        CommandScheduler::FrameObservation s_Frame = Frame(40, 20000, true);
        s_Frame.candidate_inspector = [](const CandidateQuery&) { return GoodReads(); };
        CHECK(s_Scheduler.Tick(s_Frame));
        CHECK(s_Results.size() == 1 && s_Results[0].status == Status::Expired && s_Results[0].observed.empty());
        CHECK(s_Scheduler.GetStats().candidates_inspected == 0 && s_Scheduler.GetStats().expired == 1);
    }
}
