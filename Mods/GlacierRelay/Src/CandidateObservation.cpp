#include "CandidateObservation.h"

#include <fmt/format.h>

namespace CandidateObservation
{
    using OutfitObservation::Append;

    // ---- the property-read guard -------------------------------------------------------------------

    namespace PropertyGuard
    {
        TableVerdict CheckTable(const TableFacts& p_Facts)
        {
            TableVerdict s_Verdict;

            if (!p_Facts.present)
            {
                s_Verdict.kind = TableKind::Missing;
                s_Verdict.status = FieldStatus::Unavailable;
                s_Verdict.reason = "property table null";
                return s_Verdict;
            }

            if (p_Facts.element_size == 0)
            {
                s_Verdict.kind = TableKind::Inconsistent;
                s_Verdict.status = FieldStatus::Inconsistent;
                s_Verdict.reason = "property table element size zero";
                return s_Verdict;
            }

            // A null begin with a null end is the SDK's empty array; one null end alone is not.
            if (p_Facts.begin == 0 && p_Facts.end == 0)
            {
                s_Verdict.kind = TableKind::Empty;
                s_Verdict.status = FieldStatus::Unavailable;
                s_Verdict.reason = "property table empty";
                return s_Verdict;
            }

            if (p_Facts.begin == 0 || p_Facts.end == 0 || p_Facts.end < p_Facts.begin)
            {
                s_Verdict.kind = TableKind::Inconsistent;
                s_Verdict.status = FieldStatus::Inconsistent;
                s_Verdict.reason = "property table span inconsistent";
                return s_Verdict;
            }

            const uintptr_t s_Bytes = p_Facts.end - p_Facts.begin;
            if (s_Bytes % p_Facts.element_size != 0)
            {
                s_Verdict.kind = TableKind::Inconsistent;
                s_Verdict.status = FieldStatus::Inconsistent;
                s_Verdict.reason = "property table span misaligned";
                return s_Verdict;
            }

            const size_t s_Count = static_cast<size_t>(s_Bytes / p_Facts.element_size);
            if (s_Count == 0)
            {
                s_Verdict.kind = TableKind::Empty;
                s_Verdict.status = FieldStatus::Unavailable;
                s_Verdict.reason = "property table empty";
                return s_Verdict;
            }

            if (s_Count > k_MaxPropertyCount)
            {
                s_Verdict.kind = TableKind::OverLimit;
                s_Verdict.count = s_Count;
                s_Verdict.status = FieldStatus::Unsupported;
                s_Verdict.reason = fmt::format("property table over bound ({})", k_MaxPropertyCount);
                return s_Verdict;
            }

            s_Verdict.kind = TableKind::Ok;
            s_Verdict.count = s_Count;
            s_Verdict.status = FieldStatus::Observed;
            return s_Verdict;
        }

        EntryVerdict CheckEntry(const EntryFacts& p_Facts)
        {
            EntryVerdict s_Verdict;
            const auto s_Refuse = [&](FieldStatus p_Status, std::string p_Reason) {
                s_Verdict.allowed = false;
                s_Verdict.status = p_Status;
                s_Verdict.reason = std::move(p_Reason);
                return s_Verdict;
            };

            if (!p_Facts.info_present || !p_Facts.type_present)
                return s_Refuse(FieldStatus::Unavailable, "property info null");
            if (p_Facts.has_getter)
                return s_Refuse(FieldStatus::Unsupported, "property has an engine getter");
            if (!p_Facts.type_info_present || !p_Facts.name_present)
                return s_Refuse(FieldStatus::Unavailable, "property type info null");
            if (p_Facts.name_length == 0)
                return s_Refuse(FieldStatus::Unavailable, "property type name empty");
            if (p_Facts.name_length >= k_MaxTypeNameBytes)
                return s_Refuse(FieldStatus::Unsupported, "property type name unterminated");
            if (p_Facts.name_crc != p_Facts.expected_crc || p_Facts.type_size != p_Facts.expected_size)
                return s_Refuse(FieldStatus::Unsupported, "property type differs from the expected");
            if (!p_Facts.type_registered)
                return s_Refuse(FieldStatus::Unsupported, "type not registered");
            if (!p_Facts.allocator_present)
                return s_Refuse(FieldStatus::Unavailable, "normal allocator null");
            if (!p_Facts.copy_present || !p_Facts.destruct_present)
                return s_Refuse(FieldStatus::Unavailable, "property type functions null");

            s_Verdict.allowed = true;
            s_Verdict.status = FieldStatus::Observed;
            return s_Verdict;
        }

        Decision Decide(const TableFacts& p_Table, const std::optional<EntryFacts>& p_Entry)
        {
            Decision s_Decision;
            const TableVerdict s_Table = CheckTable(p_Table);
            s_Decision.table_count = s_Table.count;

            if (s_Table.kind != TableKind::Ok)
            {
                s_Decision.stage = Stage::RejectTable;
                s_Decision.status = s_Table.status;
                s_Decision.reason = s_Table.reason;
                return s_Decision;
            }

            if (!p_Entry)
            {
                s_Decision.stage = Stage::NotDeclared;
                s_Decision.status = FieldStatus::Unavailable;
                s_Decision.reason = "property not declared on this type";
                return s_Decision;
            }

            const EntryVerdict s_Entry = CheckEntry(*p_Entry);
            if (!s_Entry.allowed)
            {
                s_Decision.stage = Stage::RejectEntry;
                s_Decision.status = s_Entry.status;
                s_Decision.reason = s_Entry.reason;
                return s_Decision;
            }

            s_Decision.stage = Stage::Read;
            s_Decision.status = FieldStatus::Observed;
            return s_Decision;
        }
    }

    const char* MemberText(Member p_Member)
    {
        switch (p_Member)
        {
            case Member::Null: return "0";
            case Member::Present: return "1";
            case Member::Valid: return "2";
        }

        return "0";
    }

    const char* SlotMatchText(SlotMatch p_Match)
    {
        switch (p_Match)
        {
            case SlotMatch::Empty: return "empty";
            case SlotMatch::Local: return "local";
            case SlotMatch::Candidate: return "candidate";
            case SlotMatch::Other: return "other";
            case SlotMatch::Inconclusive: return "inconclusive";
        }

        return "inconclusive";
    }

    // Seven significant digits (a float's), shortest form: a bounded text for every finite value
    // (at most 13 characters, e.g. "-1.234567e+10"); a non-finite value is spelled out.
    std::string Number(double p_Value)
    {
        return fmt::format("{:.7g}", p_Value);
    }

    std::string EncodeVec3(const Vec3& p_Vec)
    {
        return Number(p_Vec.x) + "," + Number(p_Vec.y) + "," + Number(p_Vec.z);
    }

    std::string EncodeLocal(const LocalTransform& p_Local)
    {
        return EncodeVec3(p_Local.translation) + "|" + Number(p_Local.rotation.x) + "," + Number(p_Local.rotation.y) + "," + Number(p_Local.rotation.z) + "," + Number(p_Local.rotation.w);
    }

    namespace
    {
        const char* Bit(bool p_Value)
        {
            return p_Value ? "1" : "0";
        }
    }

    std::string EncodeSpatialFlags(const SpatialFlags& p_Flags)
    {
        return fmt::format(
            "visible={},parent_hidden={},force_visible={},dynamic={},scene_child={},dirty={},room={},tparent={},tchildren={}",
            Bit(p_Flags.visible), Bit(p_Flags.parent_hidden), Bit(p_Flags.force_visible), Bit(p_Flags.dynamic), Bit(p_Flags.scene_child),
            Bit(p_Flags.dirty), p_Flags.room_behaviour, Bit(p_Flags.transform_parent), Bit(p_Flags.transform_children)
        );
    }

    std::string EncodeMembers(const HeroMembers& p_Members)
    {
        return fmt::format(
            "character={},animator={},morpheme={},ragdoll={},health={},kit={},camera={},input={}",
            MemberText(p_Members.character), MemberText(p_Members.animator), MemberText(p_Members.morpheme), MemberText(p_Members.ragdoll),
            MemberText(p_Members.health), MemberText(p_Members.outfit_kit), MemberText(p_Members.main_camera), MemberText(p_Members.input_processor)
        );
    }

    std::string EncodeControllers(const HeroControllers& p_Controllers)
    {
        return fmt::format(
            "guide={},ik={},cheat={},interaction={},actor_tag={}",
            MemberText(p_Controllers.guide), MemberText(p_Controllers.ik), MemberText(p_Controllers.cheat), MemberText(p_Controllers.interaction), MemberText(p_Controllers.actor_tag)
        );
    }

    std::string EncodeOutfit(const OutfitTuple& p_Outfit)
    {
        return fmt::format("{},{},{}", p_Outfit.kit_id.empty() ? "none" : p_Outfit.kit_id, p_Outfit.charset, p_Outfit.variation);
    }

    std::string EncodeSelect(bool p_Default, bool p_Preferred)
    {
        return fmt::format("default={},preferred={}", Bit(p_Default), Bit(p_Preferred));
    }

    CandidateKnowledge KnowledgeOf(bool p_Resolved, const Field<bool>& p_BrickPresent)
    {
        if (p_Resolved)
            return CandidateKnowledge::Resolved;
        if (p_BrickPresent.observed() && !*p_BrickPresent.value)
            return CandidateKnowledge::KnownAbsent;
        return CandidateKnowledge::Inconclusive;
    }

    // Local before candidate (a lookup that returned the local hero is reported as local, and as
    // not distinct by the identity block); a non-local, non-candidate entity is "other" only when
    // the candidate's identity was resolved or its absence established, else inconclusive.
    SlotMatch ClassifySlot(bool p_Present, bool p_IsLocal, bool p_IsCandidate, CandidateKnowledge p_Knowledge)
    {
        if (!p_Present)
            return SlotMatch::Empty;
        if (p_IsLocal)
            return SlotMatch::Local;
        if (p_Knowledge == CandidateKnowledge::Resolved && p_IsCandidate)
            return SlotMatch::Candidate;
        if (p_Knowledge == CandidateKnowledge::Inconclusive)
            return SlotMatch::Inconclusive;
        return SlotMatch::Other;
    }

    std::string EncodeSlots(const SlotIdentity (&p_Slots)[k_SlotCount])
    {
        std::string s_Out;
        for (size_t i = 0; i < k_SlotCount; ++i)
        {
            if (i > 0)
                s_Out += ",";
            s_Out += std::to_string(i) + ":" + SlotMatchText(p_Slots[i].match);
            if (p_Slots[i].match != SlotMatch::Empty && !p_Slots[i].hero_resolved)
                s_Out += "-nohero";
        }
        return s_Out;
    }

    std::string EncodeTargets(std::string_view p_Brick, std::string_view p_Entity, std::string_view p_Spawn)
    {
        return std::string(p_Brick) + "," + std::string(p_Entity) + "," + std::string(p_Spawn);
    }

    namespace
    {
        // A packed group as one text entry: the group's own status and reason, the grammar's text
        // only when it was observed.
        template <class T, class Encoder>
        void AppendPacked(Entries& p_Out, const char* p_Name, const Field<T>& p_Field, Encoder p_Encode)
        {
            if (p_Field.observed())
                Append(p_Out, p_Name, Field<std::string>::Observed(p_Encode(*p_Field.value)));
            else
                Append(p_Out, p_Name, Field<std::string>::Of(p_Field.status, p_Field.reason));
        }

        void AppendHero(Entries& p_Out, const char* p_Prefix, const HeroReads& p_Hero)
        {
            const std::string s_P = p_Prefix;
            AppendPacked(p_Out, (s_P + "world").c_str(), p_Hero.spatial.world, EncodeVec3);
            AppendPacked(p_Out, (s_P + "local").c_str(), p_Hero.spatial.local, EncodeLocal);
            Append(p_Out, (s_P + "parent").c_str(), p_Hero.spatial.parent);
            AppendPacked(p_Out, (s_P + "spatial_flags").c_str(), p_Hero.spatial.flags, EncodeSpatialFlags);
            AppendPacked(p_Out, (s_P + "members").c_str(), p_Hero.members, EncodeMembers);
            AppendPacked(p_Out, (s_P + "controllers").c_str(), p_Hero.controllers, EncodeControllers);
            AppendPacked(p_Out, (s_P + "outfit").c_str(), p_Hero.outfit, EncodeOutfit);
            Append(p_Out, (s_P + "active").c_str(), p_Hero.active);
        }
    }

    // 17 (identity and registry) + 8 (candidate) + 10 (spawn) + 8 (hero control) = 43 entries; the
    // scheduler adds mission_playing, scene_type, receipt_to_frame_ms and reads_on_frame (47 ≤ 48).
    void AppendCandidate(Entries& p_Out, const CandidateReads& p_Reads)
    {
        // identity block
        Append(p_Out, "generation_fresh", p_Reads.generation_fresh);
        Append(p_Out, "hero_entity_id", p_Reads.hero_entity_id);
        Append(p_Out, "hero_flags", p_Reads.hero_flags);
        Append(p_Out, "brick_count", p_Reads.bricks.count);
        Append(p_Out, "bricks_scanned", p_Reads.bricks.scanned);
        Append(p_Out, "targets", Field<std::string>::Observed(EncodeTargets(p_Reads.brick_id, p_Reads.entity_id, p_Reads.spawn_id)));
        Append(p_Out, "cand_brick_present", p_Reads.brick_present);
        Append(p_Out, "cand_subentity_count", p_Reads.lookup.subentity_count);
        Append(p_Out, "cand_index", p_Reads.lookup.index);
        Append(p_Out, "cand_index_check", p_Reads.lookup.index_check);
        Append(p_Out, "cand_resolved", p_Reads.lookup.resolved);
        Append(p_Out, "cand_hitman5", p_Reads.hitman5);
        Append(p_Out, "cand_flags", p_Reads.cand_flags);
        Append(p_Out, "cand_id_match", p_Reads.id_match);

        Field<bool> s_Distinct;
        if (p_Reads.lookup.same_entity.observed())
            s_Distinct = Field<bool>::Observed(!*p_Reads.lookup.same_entity.value);
        else
            s_Distinct = Field<bool>::Of(p_Reads.lookup.same_entity.status, p_Reads.lookup.same_entity.reason);
        Append(p_Out, "cand_distinct", s_Distinct);
        Append(p_Out, "cand_brick_owner", p_Reads.cand_brick_owner);
        Append(p_Out, "registry_slots", p_Reads.registry_slots);

        // the candidate's state
        AppendHero(p_Out, "cand_", p_Reads.candidate);

        // the spawn
        Append(p_Out, "spawn_index", p_Reads.spawn.lookup.index);
        Append(p_Out, "spawn_index_check", p_Reads.spawn.lookup.index_check);
        Append(p_Out, "spawn_resolved", p_Reads.spawn.lookup.resolved);
        Append(p_Out, "spawn_class", p_Reads.spawn.class_name);
        AppendPacked(p_Out, "spawn_world", p_Reads.spawn.world, EncodeVec3);
        Append(p_Out, "spawn_dirty", p_Reads.spawn.dirty);
        Append(p_Out, "spawn_player_id", p_Reads.spawn.player_id);
        if (p_Reads.spawn.default_spawn.observed() && p_Reads.spawn.preferred_spawn.observed())
            Append(p_Out, "spawn_select", Field<std::string>::Observed(EncodeSelect(*p_Reads.spawn.default_spawn.value, *p_Reads.spawn.preferred_spawn.value)));
        else if (!p_Reads.spawn.default_spawn.observed())
            Append(p_Out, "spawn_select", Field<std::string>::Of(p_Reads.spawn.default_spawn.status, p_Reads.spawn.default_spawn.reason));
        else
            Append(p_Out, "spawn_select", Field<std::string>::Of(p_Reads.spawn.preferred_spawn.status, p_Reads.spawn.preferred_spawn.reason));
        Append(p_Out, "spawn_hitman_ref", p_Reads.spawn.hitman_ref);
        Append(p_Out, "spawn_ref_is_cand", p_Reads.spawn.ref_is_candidate);

        // the local hero's state (the control), last
        AppendHero(p_Out, "hero_", p_Reads.hero);
    }

    namespace
    {
        template <class T>
        Field<T> Marked(FieldStatus p_Status, const char* p_Reason)
        {
            return Field<T>::Of(p_Status, p_Reason);
        }
    }

    void MarkSpatial(SpatialReads& p_Spatial, FieldStatus p_Status, const char* p_Reason)
    {
        p_Spatial.world = Marked<Vec3>(p_Status, p_Reason);
        p_Spatial.local = Marked<LocalTransform>(p_Status, p_Reason);
        p_Spatial.parent = Marked<std::string>(p_Status, p_Reason);
        p_Spatial.flags = Marked<SpatialFlags>(p_Status, p_Reason);
    }

    void MarkHero(HeroReads& p_Hero, FieldStatus p_Status, const char* p_Reason)
    {
        MarkSpatial(p_Hero.spatial, p_Status, p_Reason);
        p_Hero.members = Marked<HeroMembers>(p_Status, p_Reason);
        p_Hero.controllers = Marked<HeroControllers>(p_Status, p_Reason);
        p_Hero.outfit = Marked<OutfitTuple>(p_Status, p_Reason);
        p_Hero.active = Marked<bool>(p_Status, p_Reason);
    }

    void MarkSpawn(SpawnReads& p_Spawn, FieldStatus p_Status, const char* p_Reason)
    {
        PlayerObservation::MarkLookup(p_Spawn.lookup, p_Status, p_Reason);
        p_Spawn.class_name = Marked<std::string>(p_Status, p_Reason);
        p_Spawn.world = Marked<Vec3>(p_Status, p_Reason);
        p_Spawn.dirty = Marked<bool>(p_Status, p_Reason);
        p_Spawn.player_id = Marked<int64_t>(p_Status, p_Reason);
        p_Spawn.default_spawn = Marked<bool>(p_Status, p_Reason);
        p_Spawn.preferred_spawn = Marked<bool>(p_Status, p_Reason);
        p_Spawn.hitman_ref = Marked<std::string>(p_Status, p_Reason);
        p_Spawn.ref_is_candidate = Marked<bool>(p_Status, p_Reason);
    }

    CandidateReads AllCandidate(FieldStatus p_Status, const char* p_Reason, const CandidateQuery& p_Query)
    {
        CandidateReads s_Reads;
        s_Reads.generation_fresh = Marked<std::string>(p_Status, p_Reason);
        s_Reads.hero_entity_id = Marked<std::string>(p_Status, p_Reason);
        s_Reads.hero_flags = Marked<int64_t>(p_Status, p_Reason);
        s_Reads.bricks.count = Marked<int64_t>(p_Status, p_Reason);
        s_Reads.bricks.truncated = Marked<bool>(p_Status, p_Reason);
        s_Reads.bricks.scanned = Marked<int64_t>(p_Status, p_Reason);
        s_Reads.brick_id = p_Query.brick.value_or(k_DefaultBrick);
        s_Reads.entity_id = p_Query.entity_id.value_or(k_DefaultEntity);
        s_Reads.spawn_id = k_DefaultSpawn;
        s_Reads.brick_present = Marked<bool>(p_Status, p_Reason);
        PlayerObservation::MarkLookup(s_Reads.lookup, p_Status, p_Reason);
        s_Reads.hitman5 = Marked<bool>(p_Status, p_Reason);
        s_Reads.cand_flags = Marked<int64_t>(p_Status, p_Reason);
        s_Reads.id_match = Marked<bool>(p_Status, p_Reason);
        s_Reads.cand_brick_owner = Marked<std::string>(p_Status, p_Reason);
        s_Reads.registry_slots = Marked<std::string>(p_Status, p_Reason);
        MarkHero(s_Reads.hero, p_Status, p_Reason);
        MarkHero(s_Reads.candidate, p_Status, p_Reason);
        MarkSpawn(s_Reads.spawn, p_Status, p_Reason);
        return s_Reads;
    }
}
