#include "PlayerObservation.h"

#include <fmt/format.h>

namespace PlayerObservation
{
    using OutfitObservation::Append;
    using CommandProtocol::ObservedValue;

    const char* RoundTripText(RoundTrip p_Outcome)
    {
        switch (p_Outcome)
        {
            case RoundTrip::SameEntity: return "same_entity";
            case RoundTrip::DifferentEntity: return "different_entity";
            case RoundTrip::OwnerUnresolved: return "owner_unresolved";
            case RoundTrip::HeroNotRepresentable: return "hero_not_representable";
            case RoundTrip::FactoryUnavailable: return "factory_unavailable";
            case RoundTrip::Unsupported: return "unsupported";
        }

        return "unsupported";
    }

    // Only a demonstrably valid lookup (owner found, a template factory, a non-negative index, a
    // resolved entity) returning a different entity is the specific failure of the lookup contract;
    // every earlier gap is named for what it is, and a read that did not happen is unsupported.
    RoundTrip ClassifyRoundTrip(const LookupFacts& p_Facts)
    {
        if (!p_Facts.owner_found.observed())
            return RoundTrip::Unsupported;
        if (!*p_Facts.owner_found.value)
            return RoundTrip::OwnerUnresolved;

        if (!p_Facts.factory_available.observed() || !p_Facts.factory_template.observed())
            return RoundTrip::Unsupported;
        if (!*p_Facts.factory_available.value || !*p_Facts.factory_template.value)
            return RoundTrip::FactoryUnavailable;

        if (!p_Facts.index.observed())
            return RoundTrip::Unsupported;
        if (*p_Facts.index.value < 0)
            return RoundTrip::HeroNotRepresentable;

        if (!p_Facts.resolved.observed() || !*p_Facts.resolved.value || !p_Facts.same_entity.observed())
            return RoundTrip::Unsupported;

        return *p_Facts.same_entity.value ? RoundTrip::SameEntity : RoundTrip::DifferentEntity;
    }

    std::string Hex16(uint64_t p_Value)
    {
        return fmt::format("{:016x}", p_Value);
    }

    bool ValidHex16(std::string_view p_Text)
    {
        if (p_Text.size() != 16)
            return false;

        for (const char c : p_Text)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                return false;

        return true;
    }

    bool ParseHex16(std::string_view p_Text, uint64_t& p_Out)
    {
        if (!ValidHex16(p_Text))
            return false;

        uint64_t s_Value = 0;
        for (const char c : p_Text)
        {
            s_Value <<= 4;
            s_Value |= static_cast<uint64_t>(c >= 'a' ? c - 'a' + 10 : c - '0');
        }

        p_Out = s_Value;
        return true;
    }

    std::string ClassifyWord(uint64_t p_Word, uint64_t p_SlotsBase, uint64_t p_SlotSize, size_t p_SlotCount)
    {
        if (p_Word == 0)
            return "0";

        if (p_SlotSize > 0 && p_Word >= p_SlotsBase)
        {
            const uint64_t s_Offset = p_Word - p_SlotsBase;
            if (s_Offset < p_SlotSize * p_SlotCount && s_Offset % p_SlotSize == 0)
                return "slot" + std::to_string(s_Offset / p_SlotSize);
        }

        return fmt::format("0x{:x}", p_Word);
    }

    std::vector<std::string> GroupIds(const std::vector<std::string>& p_Ids)
    {
        std::vector<std::string> s_Fields;
        std::string s_Current;
        size_t s_InField = 0;

        for (const auto& s_Id : p_Ids)
        {
            if (s_Fields.size() >= k_MaxIdFields)
                break;

            if (s_InField == k_IdsPerField)
            {
                s_Fields.push_back(s_Current);
                s_Current.clear();
                s_InField = 0;
                if (s_Fields.size() >= k_MaxIdFields)
                    break;
            }

            if (s_InField > 0)
                s_Current += ",";
            s_Current += s_Id;
            ++s_InField;
        }

        if (s_InField > 0 && s_Fields.size() < k_MaxIdFields)
            s_Fields.push_back(s_Current);

        return s_Fields;
    }

    void Append(Entries& p_Out, const char* p_Name, const Field<double>& p_Field)
    {
        ObservedValue s_Value;
        s_Value.field_status = p_Field.status;
        s_Value.field_reason = p_Field.reason;
        s_Value.kind = ObservedValue::Kind::Null;
        if (p_Field.observed())
        {
            s_Value.kind = ObservedValue::Kind::Number;
            s_Value.number = *p_Field.value;
        }
        p_Out.emplace_back(p_Name, std::move(s_Value));
    }

    namespace
    {
        void AppendSlot(Entries& p_Out, size_t p_Index, const SlotReads& p_Slot)
        {
            const std::string s_Prefix = "slot" + std::to_string(p_Index) + "_";
            Append(p_Out, (s_Prefix + "player_id").c_str(), p_Slot.player_id);
            Append(p_Out, (s_Prefix + "local_flag").c_str(), p_Slot.local_flag);
            Append(p_Out, (s_Prefix + "connected").c_str(), p_Slot.connected_flag);
            Append(p_Out, (s_Prefix + "net_player").c_str(), p_Slot.net_player);
            Append(p_Out, (s_Prefix + "replica").c_str(), p_Slot.replica);
            Append(p_Out, (s_Prefix + "entity").c_str(), p_Slot.entity_present);
            Append(p_Out, (s_Prefix + "hero").c_str(), p_Slot.hero_resolved);
            Append(p_Out, (s_Prefix + "flags_a0").c_str(), p_Slot.flags_a0);
            Append(p_Out, (s_Prefix + "session").c_str(), p_Slot.session_id);
        }

        void AppendIdentity(Entries& p_Out, const char* p_Prefix, const EntityIdentity& p_Identity, bool p_Full)
        {
            const std::string s_P = p_Prefix;
            // The compact form (the candidate) omits the entity id: the query's echo already names
            // it, and the lookup resolves by that id.
            if (p_Full)
                Append(p_Out, (s_P + "entity_id").c_str(), p_Identity.entity_id);
            Append(p_Out, (s_P + "entity_flags").c_str(), p_Identity.entity_flags);
            if (p_Full)
            {
                Append(p_Out, (s_P + "activated").c_str(), p_Identity.activated);
                Append(p_Out, (s_P + "predeleted").c_str(), p_Identity.predeleted);
                Append(p_Out, (s_P + "owning_brick").c_str(), p_Identity.owning_brick);
            }
            Append(p_Out, (s_P + "character_id").c_str(), p_Identity.character_id);
            if (p_Full)
            {
                Append(p_Out, (s_P + "initial_outfit_id").c_str(), p_Identity.initial_outfit_id);
                Append(p_Out, (s_P + "invincible").c_str(), p_Identity.invincible);
            }
            Append(p_Out, (s_P + "x").c_str(), p_Identity.x);
            Append(p_Out, (s_P + "y").c_str(), p_Identity.y);
            Append(p_Out, (s_P + "z").c_str(), p_Identity.z);
            Append(p_Out, (s_P + "transform_dirty").c_str(), p_Identity.transform_dirty);
            if (p_Full)
                Append(p_Out, (s_P + "room_behaviour").c_str(), p_Identity.room_behaviour);
        }
    }

    // 2 + 4 × 9 + 4 = 42 entries; the scheduler adds four plain frame values (46 ≤ 48).
    void AppendRegistry(Entries& p_Out, const RegistryReads& p_Reads)
    {
        Append(p_Out, "registry_available", p_Reads.registry_available);
        Append(p_Out, "registry_words", p_Reads.words);
        for (size_t i = 0; i < k_SlotCount; ++i)
            AppendSlot(p_Out, i, p_Reads.slots[i]);
        Append(p_Out, "slot0_outfit_id", p_Reads.slots[0].outfit_id);
        Append(p_Out, "slot0_character_id", p_Reads.slots[0].character_id);
        Append(p_Out, "slot0_hero_kit_id", p_Reads.hero_kit_id);
        Append(p_Out, "slot0_matches_m5", p_Reads.slot0_matches_m5);
    }

    // At most 5 (bricks) + 14 (hero) + 7 (round trip) + 3 + 13 (candidate when present) = 42; the
    // scheduler adds four plain frame values (46 ≤ 48). Without a present candidate: 29 (+ 4).
    void AppendEntity(Entries& p_Out, const EntityReads& p_Reads)
    {
        Append(p_Out, "brick_count", p_Reads.bricks.count);
        const auto s_Groups = GroupIds(p_Reads.bricks.ids);
        for (size_t i = 0; i < s_Groups.size(); ++i)
            Append(p_Out, ("brick_ids_" + std::to_string(i + 1)).c_str(), Field<std::string>::Observed(s_Groups[i]));
        Append(p_Out, "bricks_truncated", p_Reads.bricks.truncated);

        Append(p_Out, "hero_resolved", p_Reads.hero.resolved);
        AppendIdentity(p_Out, "hero_", p_Reads.hero, true);

        Append(p_Out, "rt_outcome", Field<std::string>::Observed(RoundTripText(p_Reads.round_trip_outcome)));
        Append(p_Out, "rt_owner_found", p_Reads.round_trip.owner_found);
        Append(p_Out, "rt_factory_template", p_Reads.round_trip.factory_template);
        Append(p_Out, "rt_subentity_count", p_Reads.round_trip.subentity_count);
        Append(p_Out, "rt_index", p_Reads.round_trip.index);
        Append(p_Out, "rt_resolved", p_Reads.round_trip.resolved);
        Append(p_Out, "rt_same_entity", p_Reads.round_trip.same_entity);

        if (p_Reads.candidate)
        {
            const auto& s_Candidate = *p_Reads.candidate;
            Append(p_Out, "cand_brick", Field<std::string>::Observed(s_Candidate.brick_id));
            Append(p_Out, "cand_entity_id", Field<std::string>::Observed(s_Candidate.entity_id));
            Append(p_Out, "cand_brick_present", s_Candidate.brick_present);

            if (s_Candidate.brick_present.observed() && *s_Candidate.brick_present.value)
            {
                Append(p_Out, "cand_factory_template", s_Candidate.lookup.factory_template);
                Append(p_Out, "cand_subentity_count", s_Candidate.lookup.subentity_count);
                Append(p_Out, "cand_index", s_Candidate.lookup.index);
                Append(p_Out, "cand_resolved", s_Candidate.lookup.resolved);
                Append(p_Out, "cand_hitman5", s_Candidate.hitman5_resolved);
                Append(p_Out, "cand_spatial", s_Candidate.spatial_resolved);

                Field<bool> s_Distinct;
                if (s_Candidate.lookup.same_entity.observed())
                    s_Distinct = Field<bool>::Observed(!*s_Candidate.lookup.same_entity.value);
                else
                    s_Distinct = Field<bool>::Of(s_Candidate.lookup.same_entity.status, s_Candidate.lookup.same_entity.reason);
                Append(p_Out, "cand_distinct_from_local", s_Distinct);

                AppendIdentity(p_Out, "cand_", s_Candidate.identity, false);
            }
        }
    }

    namespace
    {
        template <class T>
        Field<T> Marked(FieldStatus p_Status, const char* p_Reason)
        {
            return Field<T>::Of(p_Status, p_Reason);
        }
    }

    void MarkIdentity(EntityIdentity& p_Identity, FieldStatus p_Status, const char* p_Reason)
    {
        p_Identity.resolved = Marked<bool>(p_Status, p_Reason);
        p_Identity.entity_id = Marked<std::string>(p_Status, p_Reason);
        p_Identity.entity_flags = Marked<int64_t>(p_Status, p_Reason);
        p_Identity.activated = Marked<bool>(p_Status, p_Reason);
        p_Identity.predeleted = Marked<bool>(p_Status, p_Reason);
        p_Identity.owning_brick = Marked<std::string>(p_Status, p_Reason);
        p_Identity.character_id = Marked<std::string>(p_Status, p_Reason);
        p_Identity.initial_outfit_id = Marked<std::string>(p_Status, p_Reason);
        p_Identity.invincible = Marked<bool>(p_Status, p_Reason);
        p_Identity.x = Marked<double>(p_Status, p_Reason);
        p_Identity.y = Marked<double>(p_Status, p_Reason);
        p_Identity.z = Marked<double>(p_Status, p_Reason);
        p_Identity.transform_dirty = Marked<bool>(p_Status, p_Reason);
        p_Identity.room_behaviour = Marked<int64_t>(p_Status, p_Reason);
    }

    void MarkLookup(LookupFacts& p_Facts, FieldStatus p_Status, const char* p_Reason)
    {
        p_Facts.owner_found = Marked<bool>(p_Status, p_Reason);
        p_Facts.factory_available = Marked<bool>(p_Status, p_Reason);
        p_Facts.factory_template = Marked<bool>(p_Status, p_Reason);
        p_Facts.subentity_count = Marked<int64_t>(p_Status, p_Reason);
        p_Facts.index = Marked<int64_t>(p_Status, p_Reason);
        p_Facts.resolved = Marked<bool>(p_Status, p_Reason);
        p_Facts.same_entity = Marked<bool>(p_Status, p_Reason);
    }

    RegistryReads AllRegistry(FieldStatus p_Status, const char* p_Reason)
    {
        RegistryReads s_Reads;
        s_Reads.registry_available = Marked<bool>(p_Status, p_Reason);
        s_Reads.words = Marked<std::string>(p_Status, p_Reason);
        for (auto& s_Slot : s_Reads.slots)
        {
            s_Slot.player_id = Marked<int64_t>(p_Status, p_Reason);
            s_Slot.local_flag = Marked<bool>(p_Status, p_Reason);
            s_Slot.connected_flag = Marked<bool>(p_Status, p_Reason);
            s_Slot.net_player = Marked<bool>(p_Status, p_Reason);
            s_Slot.replica = Marked<bool>(p_Status, p_Reason);
            s_Slot.entity_present = Marked<bool>(p_Status, p_Reason);
            s_Slot.hero_resolved = Marked<bool>(p_Status, p_Reason);
            s_Slot.flags_a0 = Marked<int64_t>(p_Status, p_Reason);
            s_Slot.session_id = Marked<std::string>(p_Status, p_Reason);
            s_Slot.outfit_id = Marked<std::string>(p_Status, p_Reason);
            s_Slot.character_id = Marked<std::string>(p_Status, p_Reason);
        }
        s_Reads.hero_kit_id = Marked<std::string>(p_Status, p_Reason);
        s_Reads.slot0_matches_m5 = Marked<bool>(p_Status, p_Reason);
        return s_Reads;
    }

    EntityReads AllEntity(FieldStatus p_Status, const char* p_Reason, const std::optional<EntityQuery>& p_Query)
    {
        EntityReads s_Reads;
        s_Reads.bricks.count = Marked<int64_t>(p_Status, p_Reason);
        s_Reads.bricks.truncated = Marked<bool>(p_Status, p_Reason);
        MarkIdentity(s_Reads.hero, p_Status, p_Reason);
        MarkLookup(s_Reads.round_trip, p_Status, p_Reason);
        s_Reads.round_trip_outcome = RoundTrip::Unsupported;

        if (p_Query)
        {
            CandidateReads s_Candidate;
            s_Candidate.brick_id = p_Query->brick.value_or("");
            s_Candidate.entity_id = p_Query->entity_id.value_or("");
            s_Candidate.brick_present = Marked<bool>(p_Status, p_Reason);
            MarkLookup(s_Candidate.lookup, p_Status, p_Reason);
            MarkIdentity(s_Candidate.identity, p_Status, p_Reason);
            s_Candidate.hitman5_resolved = Marked<bool>(p_Status, p_Reason);
            s_Candidate.spatial_resolved = Marked<bool>(p_Status, p_Reason);
            s_Reads.candidate = s_Candidate;
        }

        return s_Reads;
    }
}
