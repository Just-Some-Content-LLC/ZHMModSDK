#pragma once

#include <cstdint>
#include <string>

#include "CandidateObservation.h"
#include "CommandProtocol.h"
#include "FloatWords.h"
#include "OutfitObservation.h"
#include "PlayerObservation.h"

// The exact transform snapshot of M6-F (glacier-relay docs/design/M6D2_NEXT_STEP_DESIGN.md
// sections 12.3–12.4): what relay.inspect.candidate_transform v1 reports and what the transform
// apply reads before and after its one call, as plain owned values with an explicit per-field
// status. The candidate is the compiled historical pair (hitmen.brick / Agent48); nothing here is
// addressable by a peer, and nothing here is an engine type, a pointer, an entity reference or an
// address. The Glacier-facing reader (SceneObservation::ObserveCandidateTransform) fills these from
// guarded reads on the frame — the stored m_mTransform (twelve floats, never through the refreshing
// getter), the stored parent-relative translation and rotation (seven floats), the dirty flag, the
// parent's authored id and the visibility bits — and keeps no reference beyond the frame except,
// for the transform applier only, into the handles it releases before the frame ends.
//
// The exact words are the evidence; the seven-digit texts beside them are for reading only.
namespace TransformObservation
{
    using OutfitObservation::Field;
    using OutfitObservation::Entries;
    using FieldStatus = CommandProtocol::FieldStatus;
    using FloatWords::Words12;
    using FloatWords::Words7;

    constexpr const char* k_Brick = CandidateObservation::k_DefaultBrick;   // 007c57a6ea581f8c (compiled; not a parameter)
    constexpr const char* k_Entity = CandidateObservation::k_DefaultEntity; // feede715906f747f (compiled; not a parameter)
    constexpr int64_t k_SnapshotVersion = 1;
    constexpr float k_LiftZ = 0.25f;               // the v1 compiled displacement: world z, binary32 addition
    constexpr float k_StepY = 0.25f;               // the v2 compiled displacement (M6-G1): world y, binary32 addition
    constexpr float k_DriftToleranceM = 1.0e-4f;    // the declared comparison tolerance (section 12.8); never equality

    // One entity's exact stored transform on one frame.
    struct Snapshot
    {
        Field<Words12> world;                          // m_mTransform as stored (XAxis, YAxis, ZAxis, Trans)
        Field<Words7> local;                           // m_vObjectToParentTranslation, m_vObjectToParentRotation
        Field<bool> dirty;                             // m_bWorldTransformDirty
        Field<std::string> parent;                     // m_eidParent's authored id (16 hex) or "none"
        Field<CandidateObservation::SpatialFlags> flags;
    };

    struct Reads
    {
        Field<std::string> generation_fresh; // registry slot 0's session id read twice on this frame
        std::string brick_id = k_Brick;      // the compiled targets, echoed
        std::string entity_id = k_Entity;

        // the candidate's identity block (M6-D1a semantics; the same lookup and bounds)
        Field<bool> brick_present;
        PlayerObservation::LookupFacts lookup; // same_entity: the lookup returned the local hero (not distinct)
        Field<bool> hitman5;
        Field<int64_t> cand_flags;
        Field<bool> id_match;
        Field<std::string> cand_brick_owner;
        Field<bool> spatial;                   // the ZSpatialEntity interface resolved on the candidate
        Field<bool> spatial_distinct;          // its interface pointer differs from the local hero's (compared on the frame; never kept)
        Field<std::string> registry_slots;     // "0:T,1:T,2:T,3:T" (CandidateObservation::EncodeSlots)
        Field<bool> candidate_in_registry;     // any slot classified candidate, or any populated slot inconclusive
        Snapshot candidate;

        // the local hero (registry slot 0) as the control
        Field<std::string> hero_entity_id;
        Field<int64_t> hero_flags;
        Snapshot hero;

        // the apply's post read only: the re-resolved candidate is the entity the pre read resolved
        Field<bool> same_entity_as_before;

        uint32_t thread_id = 0;
    };

    // The compiled forward target: every word of the preimage copied, Trans.z replaced by
    // preimage.Trans.z + k_LiftZ in binary32 arithmetic (round to nearest).
    Words12 LiftTarget(const Words12& p_Preimage);

    // The v2 compiled forward target (M6-G1): every word of the preimage copied, Trans.y (word 10)
    // replaced by preimage.Trans.y + k_StepY in binary32 arithmetic (round to nearest). World y is
    // not a model-relative direction; the unchanged z and rotation words are a request, never a
    // promise about the engine's post-state.
    Words12 StepYTarget(const Words12& p_Preimage);

    // The 30 inspection entries in a fixed order (the scheduler adds mission_playing, scene_type,
    // receipt_to_frame_ms and reads_on_frame: 34 <= 48): the generation, the targets and version,
    // the identity block, the registry, the candidate's exact snapshot, then the hero control last
    // so that the protocol's reduction removes the control before the candidate and never the
    // identity.
    void AppendInspection(Entries& p_Out, const Reads& p_Reads);

    // Encoding helpers shared with the apply result.
    void AppendSnapshot(Entries& p_Out, const char* p_Prefix, const Snapshot& p_Snapshot, bool p_WithFlags);
    void Append(Entries& p_Out, const char* p_Name, const Field<Words12>& p_Field);
    void Append(Entries& p_Out, const char* p_Name, const Field<Words7>& p_Field);

    // Every field marked with one status and reason (no inspector, faults, the latched path).
    void MarkSnapshot(Snapshot& p_Snapshot, FieldStatus p_Status, const char* p_Reason);
    Reads AllUnavailable(FieldStatus p_Status, const char* p_Reason);
}
