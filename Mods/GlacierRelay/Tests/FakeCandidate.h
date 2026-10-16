#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

#include <Windows.h>

#include "CandidateObservation.h"
#include "PlayerObservation.h"

// M6-D1a: the FAKE candidate, spawn and registry-identity state the command probe's candidate
// inspector reports (relay.inspect.candidate), scripted per candidate-mode. Every value is scripted
// here and is never an observation of HITMAN; no pointer exists here; no engine, no SDK function.
// The shapes mirror what the DLL's reader would produce in each situation, built with the same
// pure rules (CandidatePresence, the index checks, EncodeSlots) so BEAM's decoding and the
// operator tooling are exercised against the real encoding of each case.
//
// The identity and transform values agree with the M6-B1 entity fake (CommandProbe.cpp: the hero
// feed9a0d70d546f4 with flags 7 at (-0.035, -3.832, -0.413); the candidate at index 0 of 3 with flags
// 7 at (-40.99882, -24.293657, 0.4925565)) so that the operator cross-check between the two
// inspections (xcheck.py --candidate) is exercised on consistent FAKE data in the tooling dry run.
//
// candidate-mode: "default" (the candidate brick absent on a complete scan: 5 bricks at the menu,
// 23 in a mission), "candidate-present" (present, resolved, hero interface, every member present,
// m_bActive true, the spawn resolved as ZHeroSpawn referencing the candidate, player id 2, the
// registry holding the local hero only), "candidate-incomplete" (as present, members partially
// missing, m_bActive false), "candidate-in-slot" (as present, registry slot 1 is the candidate),
// "candidate-in-slot-nohero" (slot 1 is the candidate without a resolved hero interface),
// "spawn-other" (the spawn references a different entity), "spawn-missing" (the spawn id not a
// sub-entity), "entity-missing" (the brick present, the candidate id not a sub-entity),
// "hero-interface-missing" (the candidate resolved without ZHitman5), "parent-none", "parent-
// unavailable", "dirty-transform", "hidden", "active-unavailable" (the property not declared),
// "active-getter" (the property has an engine getter: unsupported), "registry-unavailable",
// "count-invalid", "index-at-count", "bricks-oversized" (70 bricks, absence not established, a
// populated slot inconclusive), "texts-maximal" (every text and number at its longest; fits
// whole), "reasons-maximal" (every field unavailable with a 40-byte reason: the first reduction
// stage), "scene-unavailable" (no scene context: the candidate block unavailable, the local hero
// still read). Pathological shapes by design; never HITMAN state.
namespace FakeCandidate
{
    using OutfitObservation::Field;
    using CommandProtocol::FieldStatus;
    using namespace CandidateObservation;

    constexpr const char* k_FakeSuit = "874c4c48-0a8b-49e9-883e-49fc5f1fb051";
    constexpr const char* k_FakeHeroId = "feed9a0d70d546f4";     // a FAKE authored id for the local hero
    constexpr const char* k_FakeOtherId = "feed000000000bad";    // a FAKE authored id that is neither hero
    constexpr const char* k_FakeParentId = "0000000000000001";   // a FAKE parent entity id
    constexpr const char* k_FakeHitmenBrick = "007c57a6ea581f8c"; // a FAKE stand-in for the historical brick id
    constexpr const char* k_FakeAgent48 = "feede715906f747f";

    inline HeroReads Hero(double p_X, double p_Y, double p_Z, bool p_Complete, bool p_Active)
    {
        HeroReads s_Hero;
        Vec3 s_World;
        s_World.x = p_X;
        s_World.y = p_Y;
        s_World.z = p_Z;
        s_Hero.spatial.world = Field<Vec3>::Observed(s_World);
        LocalTransform s_Local;
        s_Local.rotation.w = 1;
        s_Hero.spatial.local = Field<LocalTransform>::Observed(s_Local);
        s_Hero.spatial.parent = Field<std::string>::Observed(k_FakeParentId);
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
        OutfitTuple s_Outfit;
        s_Outfit.kit_id = k_FakeSuit;
        s_Hero.outfit = Field<OutfitTuple>::Observed(s_Outfit);
        s_Hero.active = Field<bool>::Observed(p_Active);
        return s_Hero;
    }

    inline PlayerObservation::LookupFacts Lookup(int64_t p_Count, int64_t p_Index)
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

    inline SpawnReads Spawn(const std::string& p_RefId, bool p_RefIsCandidate)
    {
        SpawnReads s_Spawn;
        s_Spawn.lookup = Lookup(3, 2);
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

    inline void MarkCandidateAbsent(CandidateReads& p_Reads, const std::string& p_Why)
    {
        PlayerObservation::MarkLookup(p_Reads.lookup, FieldStatus::Unavailable, p_Why.c_str());
        p_Reads.hitman5 = Field<bool>::Of(FieldStatus::Unavailable, p_Why);
        p_Reads.cand_flags = Field<int64_t>::Of(FieldStatus::Unavailable, p_Why);
        p_Reads.id_match = Field<bool>::Of(FieldStatus::Unavailable, p_Why);
        p_Reads.cand_brick_owner = Field<std::string>::Of(FieldStatus::Unavailable, p_Why);
        MarkHero(p_Reads.candidate, FieldStatus::Unavailable, p_Why.c_str());
        MarkSpawn(p_Reads.spawn, FieldStatus::Unavailable, p_Why.c_str());
    }

    inline Field<std::string> Slots(SlotMatch p_1, bool p_Hero1 = true)
    {
        SlotIdentity s_Slots[k_SlotCount];
        s_Slots[0].match = SlotMatch::Local;
        s_Slots[0].hero_resolved = true;
        s_Slots[1].match = p_1;
        s_Slots[1].hero_resolved = p_Hero1;
        return Field<std::string>::Observed(EncodeSlots(s_Slots));
    }

    // The scripted read set for one candidate-mode on one fake frame.
    inline CandidateReads Reads(const std::string& p_Mode, bool p_Playing, const std::string& p_Generation, const CandidateQuery& p_Query)
    {
        const std::string s_MaxReason(CommandProtocol::k_MaxObservedReasonBytes, 'r');
        if (p_Mode == "reasons-maximal")
        {
            CandidateReads s_All = AllCandidate(FieldStatus::Unavailable, s_MaxReason.c_str(), p_Query);
            s_All.thread_id = GetCurrentThreadId();
            return s_All;
        }

        CandidateReads s_Reads;
        s_Reads.thread_id = GetCurrentThreadId();
        s_Reads.generation_fresh = p_Generation.empty() ? Field<std::string>::Of(FieldStatus::Unavailable, "registry session id empty or implausible") : Field<std::string>::Observed(p_Generation);
        s_Reads.brick_id = p_Query.brick.value_or(k_FakeHitmenBrick);
        s_Reads.entity_id = p_Query.entity_id.value_or(k_FakeAgent48);
        s_Reads.spawn_id = k_DefaultSpawn;

        // the local hero (the control): present at the menu and in a mission (experiment 4, F5)
        const bool s_RegistryUnavailable = p_Mode == "registry-unavailable";
        if (s_RegistryUnavailable)
        {
            s_Reads.generation_fresh = Field<std::string>::Of(FieldStatus::Unavailable, "fake registry global null");
            s_Reads.hero_entity_id = Field<std::string>::Of(FieldStatus::Unavailable, "fake registry global null");
            s_Reads.hero_flags = Field<int64_t>::Of(FieldStatus::Unavailable, "fake registry global null");
            MarkHero(s_Reads.hero, FieldStatus::Unavailable, "fake registry global null");
        }
        else
        {
            s_Reads.hero_entity_id = Field<std::string>::Observed(k_FakeHeroId);
            s_Reads.hero_flags = Field<int64_t>::Observed(7);
            s_Reads.hero = Hero(-0.035, -3.832, -0.413, true, true);
        }

        if (p_Mode == "scene-unavailable")
        {
            s_Reads.bricks.count = Field<int64_t>::Of(FieldStatus::Unavailable, "fake scene context null");
            s_Reads.bricks.truncated = Field<bool>::Of(FieldStatus::Unavailable, "fake scene context null");
            s_Reads.bricks.scanned = Field<int64_t>::Of(FieldStatus::Unavailable, "fake scene context null");
            s_Reads.brick_present = Field<bool>::Of(FieldStatus::Unavailable, "fake scene context null");
            MarkCandidateAbsent(s_Reads, "fake scene context null");
            s_Reads.registry_slots = Slots(SlotMatch::Empty);
            return s_Reads;
        }

        // the bricks: 5 at the menu, 23 in a mission (+1 when the candidate's brick is present), or 70
        const bool s_Present = p_Mode != "default" && p_Mode != "bricks-oversized" && p_Mode != "registry-unavailable";
        size_t s_Count = p_Playing ? 23 : 5;
        if (p_Mode == "bricks-oversized")
            s_Count = 70;
        if (s_Present)
            ++s_Count;
        s_Reads.bricks.count = Field<int64_t>::Observed(static_cast<int64_t>(s_Count));
        s_Reads.bricks.truncated = Field<bool>::Observed(s_Count > PlayerObservation::k_MaxBrickIds);
        s_Reads.bricks.scanned = Field<int64_t>::Observed(static_cast<int64_t>(std::min(s_Count, PlayerObservation::k_MaxBricksScanned)));

        if (!s_Present)
        {
            s_Reads.brick_present = PlayerObservation::CandidatePresence(false, s_Reads.bricks);
            const std::string s_Why = s_Reads.brick_present.observed() ? "brick not loaded" : "brick scan incomplete";
            MarkCandidateAbsent(s_Reads, s_Why);
            if (s_RegistryUnavailable)
                s_Reads.registry_slots = Field<std::string>::Of(FieldStatus::Unavailable, "fake registry global null");
            else if (p_Mode == "bricks-oversized")
                s_Reads.registry_slots = Slots(SlotMatch::Inconclusive); // a populated slot cannot be compared: absence not established
            else
                s_Reads.registry_slots = Slots(SlotMatch::Empty);
            return s_Reads;
        }

        s_Reads.brick_present = PlayerObservation::CandidatePresence(true, s_Reads.bricks);

        if (p_Mode == "entity-missing" || p_Mode == "count-invalid" || p_Mode == "index-at-count")
        {
            s_Reads.lookup = p_Mode == "entity-missing" ? Lookup(3, -1) : p_Mode == "count-invalid" ? Lookup(-1, 0) : Lookup(3, 3);
            const char* s_Why = "candidate entity not resolved";
            s_Reads.hitman5 = Field<bool>::Of(FieldStatus::Unavailable, s_Why);
            s_Reads.cand_flags = Field<int64_t>::Of(FieldStatus::Unavailable, s_Why);
            s_Reads.id_match = Field<bool>::Of(FieldStatus::Unavailable, s_Why);
            s_Reads.cand_brick_owner = Field<std::string>::Of(FieldStatus::Unavailable, s_Why);
            MarkHero(s_Reads.candidate, FieldStatus::Unavailable, s_Why);
            s_Reads.spawn = Spawn(k_FakeAgent48, false);
            s_Reads.spawn.ref_is_candidate = Field<bool>::Of(FieldStatus::Unavailable, s_Why);
            s_Reads.registry_slots = Slots(SlotMatch::Empty);
            return s_Reads;
        }

        // the candidate resolved (index 0 of 3, flags 7, at the M6-B1 fake's transform)
        s_Reads.lookup = Lookup(3, 0);
        const bool s_Interface = p_Mode != "hero-interface-missing";
        s_Reads.hitman5 = Field<bool>::Observed(s_Interface);
        s_Reads.cand_flags = Field<int64_t>::Observed(7);
        s_Reads.id_match = Field<bool>::Observed(true);
        s_Reads.cand_brick_owner = Field<std::string>::Observed(s_Reads.brick_id);
        const bool s_Complete = p_Mode != "candidate-incomplete";
        s_Reads.candidate = Hero(-40.99882, -24.293657, 0.4925565, s_Complete, s_Complete);
        if (!s_Interface)
        {
            s_Reads.candidate.members = Field<HeroMembers>::Of(FieldStatus::Unavailable, "hero interface not resolved");
            s_Reads.candidate.controllers = Field<HeroControllers>::Of(FieldStatus::Unavailable, "hero interface not resolved");
            s_Reads.candidate.outfit = Field<OutfitTuple>::Of(FieldStatus::Unavailable, "hero interface not resolved");
            s_Reads.candidate.active = Field<bool>::Of(FieldStatus::Unavailable, "hero interface not resolved");
        }
        if (p_Mode == "parent-none")
            s_Reads.candidate.spatial.parent = Field<std::string>::Observed("none");
        if (p_Mode == "parent-unavailable")
            s_Reads.candidate.spatial.parent = Field<std::string>::Of(FieldStatus::Unavailable, "entity type null");
        if (p_Mode == "dirty-transform" || p_Mode == "hidden")
        {
            SpatialFlags s_Flags = *s_Reads.candidate.spatial.flags.value;
            if (p_Mode == "dirty-transform")
                s_Flags.dirty = true;
            else
            {
                s_Flags.visible = false;
                s_Flags.parent_hidden = true;
            }
            s_Reads.candidate.spatial.flags = Field<SpatialFlags>::Observed(s_Flags);
        }
        if (p_Mode == "active-unavailable")
            s_Reads.candidate.active = Field<bool>::Of(FieldStatus::Unavailable, "property not declared on this type");
        if (p_Mode == "active-getter")
            s_Reads.candidate.active = Field<bool>::Of(FieldStatus::Unsupported, "property has an engine getter");

        // the spawn
        if (p_Mode == "spawn-missing")
        {
            MarkSpawn(s_Reads.spawn, FieldStatus::Unavailable, "spawn entity not resolved");
            s_Reads.spawn.lookup = Lookup(3, -1);
        }
        else if (p_Mode == "spawn-other")
            s_Reads.spawn = Spawn(k_FakeOtherId, false);
        else
            s_Reads.spawn = Spawn(s_Reads.entity_id, true);

        // the registry
        if (p_Mode == "candidate-in-slot")
            s_Reads.registry_slots = Slots(SlotMatch::Candidate);
        else if (p_Mode == "candidate-in-slot-nohero")
            s_Reads.registry_slots = Slots(SlotMatch::Candidate, false);
        else
            s_Reads.registry_slots = Slots(SlotMatch::Empty);

        if (p_Mode == "texts-maximal")
        {
            // every text and number at its longest rendering: the line must still fit whole
            s_Reads.generation_fresh = Field<std::string>::Observed(std::string(64, 'g'));
            s_Reads.bricks.count = Field<int64_t>::Observed(1 << 20);
            s_Reads.bricks.scanned = Field<int64_t>::Observed(64);
            s_Reads.lookup = Lookup(1 << 20, (1 << 20) - 1);
            s_Reads.hero_flags = Field<int64_t>::Observed(127);
            s_Reads.cand_flags = Field<int64_t>::Observed(127);
            SlotIdentity s_Slots[k_SlotCount];
            for (auto& s_Slot : s_Slots)
                s_Slot.match = SlotMatch::Inconclusive;
            s_Reads.registry_slots = Field<std::string>::Observed(EncodeSlots(s_Slots));
            auto s_Max = [](HeroReads p_Hero) {
                Vec3 s_Far;
                s_Far.x = s_Far.y = s_Far.z = -123456789.0;
                p_Hero.spatial.world = Field<Vec3>::Observed(s_Far);
                LocalTransform s_Local;
                s_Local.translation = s_Far;
                s_Local.rotation.x = s_Local.rotation.y = s_Local.rotation.z = s_Local.rotation.w = -0.70710678;
                p_Hero.spatial.local = Field<LocalTransform>::Observed(s_Local);
                SpatialFlags s_Flags;
                s_Flags.visible = s_Flags.parent_hidden = s_Flags.force_visible = s_Flags.dynamic = s_Flags.scene_child = s_Flags.dirty = s_Flags.transform_parent = s_Flags.transform_children = true;
                s_Flags.room_behaviour = 3;
                p_Hero.spatial.flags = Field<SpatialFlags>::Observed(s_Flags);
                OutfitTuple s_Outfit;
                s_Outfit.kit_id = k_FakeSuit;
                s_Outfit.charset = 15;
                s_Outfit.variation = 63;
                p_Hero.outfit = Field<OutfitTuple>::Observed(s_Outfit);
                return p_Hero;
            };
            s_Reads.hero = s_Max(s_Reads.hero);
            s_Reads.candidate = s_Max(s_Reads.candidate);
            s_Reads.spawn = Spawn(k_FakeOtherId, false);
            s_Reads.spawn.lookup = Lookup(1 << 20, (1 << 20) - 2);
            s_Reads.spawn.class_name = Field<std::string>::Observed(std::string(k_MaxClassNameBytes, 'Z'));
            Vec3 s_Far;
            s_Far.x = s_Far.y = s_Far.z = -123456789.0;
            s_Reads.spawn.world = Field<Vec3>::Observed(s_Far);
            s_Reads.spawn.player_id = Field<int64_t>::Observed(-2147483648LL);
        }

        return s_Reads;
    }
}
