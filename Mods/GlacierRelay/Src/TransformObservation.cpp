#include "TransformObservation.h"

namespace TransformObservation
{
    using OutfitObservation::Append;

    Words12 LiftTarget(const Words12& p_Preimage)
    {
        Words12 s_Target = p_Preimage;
        const float s_Z = FloatWords::ToFloat(p_Preimage.w[11]);
        s_Target.w[11] = FloatWords::ToWord(s_Z + k_LiftZ);
        return s_Target;
    }

    Words12 StepYTarget(const Words12& p_Preimage)
    {
        Words12 s_Target = p_Preimage;
        const float s_Y = FloatWords::ToFloat(p_Preimage.w[10]);
        s_Target.w[10] = FloatWords::ToWord(s_Y + k_StepY);
        return s_Target;
    }

    void Append(Entries& p_Out, const char* p_Name, const Field<Words12>& p_Field)
    {
        if (p_Field.observed())
            OutfitObservation::Append(p_Out, p_Name, Field<std::string>::Observed(FloatWords::Encode(*p_Field.value)));
        else
            OutfitObservation::Append(p_Out, p_Name, Field<std::string>::Of(p_Field.status, p_Field.reason));
    }

    void Append(Entries& p_Out, const char* p_Name, const Field<Words7>& p_Field)
    {
        if (p_Field.observed())
            OutfitObservation::Append(p_Out, p_Name, Field<std::string>::Observed(FloatWords::Encode(*p_Field.value)));
        else
            OutfitObservation::Append(p_Out, p_Name, Field<std::string>::Of(p_Field.status, p_Field.reason));
    }

    namespace
    {
        Field<std::string> TextOf(const Field<Words12>& p_Field)
        {
            return p_Field.observed() ? Field<std::string>::Observed(FloatWords::TranslationText(*p_Field.value)) : Field<std::string>::Of(p_Field.status, p_Field.reason);
        }

        Field<std::string> TextOf(const Field<Words7>& p_Field)
        {
            return p_Field.observed() ? Field<std::string>::Observed(FloatWords::LocalText(*p_Field.value)) : Field<std::string>::Of(p_Field.status, p_Field.reason);
        }
    }

    void AppendSnapshot(Entries& p_Out, const char* p_Prefix, const Snapshot& p_Snapshot, bool p_WithFlags)
    {
        const std::string s_P = p_Prefix;
        Append(p_Out, (s_P + "world_words").c_str(), p_Snapshot.world);
        Append(p_Out, (s_P + "local_words").c_str(), p_Snapshot.local);
        OutfitObservation::Append(p_Out, (s_P + "dirty").c_str(), p_Snapshot.dirty);
        OutfitObservation::Append(p_Out, (s_P + "parent").c_str(), p_Snapshot.parent);
        OutfitObservation::Append(p_Out, (s_P + "world").c_str(), TextOf(p_Snapshot.world));
        if (p_WithFlags)
        {
            OutfitObservation::Append(p_Out, (s_P + "local").c_str(), TextOf(p_Snapshot.local));
            if (p_Snapshot.flags.observed())
                OutfitObservation::Append(p_Out, (s_P + "spatial_flags").c_str(), Field<std::string>::Observed(CandidateObservation::EncodeSpatialFlags(*p_Snapshot.flags.value)));
            else
                OutfitObservation::Append(p_Out, (s_P + "spatial_flags").c_str(), Field<std::string>::Of(p_Snapshot.flags.status, p_Snapshot.flags.reason));
        }
    }

    // 1 + 2 + 10 + 2 + 2 + 7 (candidate: words ×2, dirty, parent, world, local, flags) + 2 + 5 (hero: words ×2, dirty, parent, world) = 31; the
    // scheduler adds mission_playing, scene_type, receipt_to_frame_ms and reads_on_frame (35 <= 48).
    void AppendInspection(Entries& p_Out, const Reads& p_Reads)
    {
        Append(p_Out, "generation_fresh", p_Reads.generation_fresh);
        Append(p_Out, "targets", Field<std::string>::Observed(p_Reads.brick_id + "," + p_Reads.entity_id));
        Append(p_Out, "snapshot_version", Field<int64_t>::Observed(k_SnapshotVersion));

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

        Append(p_Out, "cand_spatial", p_Reads.spatial);
        Append(p_Out, "cand_spatial_distinct", p_Reads.spatial_distinct);
        Append(p_Out, "registry_slots", p_Reads.registry_slots);
        Append(p_Out, "cand_in_registry", p_Reads.candidate_in_registry);

        AppendSnapshot(p_Out, "cand_", p_Reads.candidate, true);

        Append(p_Out, "hero_entity_id", p_Reads.hero_entity_id);
        Append(p_Out, "hero_flags", p_Reads.hero_flags);
        AppendSnapshot(p_Out, "hero_", p_Reads.hero, false);
    }

    void MarkSnapshot(Snapshot& p_Snapshot, FieldStatus p_Status, const char* p_Reason)
    {
        p_Snapshot.world = Field<Words12>::Of(p_Status, p_Reason);
        p_Snapshot.local = Field<Words7>::Of(p_Status, p_Reason);
        p_Snapshot.dirty = Field<bool>::Of(p_Status, p_Reason);
        p_Snapshot.parent = Field<std::string>::Of(p_Status, p_Reason);
        p_Snapshot.flags = Field<CandidateObservation::SpatialFlags>::Of(p_Status, p_Reason);
    }

    Reads AllUnavailable(FieldStatus p_Status, const char* p_Reason)
    {
        Reads s_Reads;
        s_Reads.generation_fresh = Field<std::string>::Of(p_Status, p_Reason);
        s_Reads.brick_present = Field<bool>::Of(p_Status, p_Reason);
        PlayerObservation::MarkLookup(s_Reads.lookup, p_Status, p_Reason);
        s_Reads.hitman5 = Field<bool>::Of(p_Status, p_Reason);
        s_Reads.cand_flags = Field<int64_t>::Of(p_Status, p_Reason);
        s_Reads.id_match = Field<bool>::Of(p_Status, p_Reason);
        s_Reads.cand_brick_owner = Field<std::string>::Of(p_Status, p_Reason);
        s_Reads.spatial = Field<bool>::Of(p_Status, p_Reason);
        s_Reads.spatial_distinct = Field<bool>::Of(p_Status, p_Reason);
        s_Reads.registry_slots = Field<std::string>::Of(p_Status, p_Reason);
        s_Reads.candidate_in_registry = Field<bool>::Of(p_Status, p_Reason);
        MarkSnapshot(s_Reads.candidate, p_Status, p_Reason);
        s_Reads.hero_entity_id = Field<std::string>::Of(p_Status, p_Reason);
        s_Reads.hero_flags = Field<int64_t>::Of(p_Status, p_Reason);
        MarkSnapshot(s_Reads.hero, p_Status, p_Reason);
        s_Reads.same_entity_as_before = Field<bool>::Of(p_Status, p_Reason);
        return s_Reads;
    }
}
