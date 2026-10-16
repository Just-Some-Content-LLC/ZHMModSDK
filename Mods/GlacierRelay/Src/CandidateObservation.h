#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "CommandProtocol.h"
#include "OutfitObservation.h"
#include "PlayerObservation.h"

// What the M6-D1a read-only candidate-hero inspection can report (relay.inspect.candidate v1), as
// plain owned values with an explicit per-field status, in the M6-B1 pattern (PlayerObservation).
// Nothing here is an engine type, a pointer, an entity reference, a resource handle or an address:
// the Glacier-facing reader (SceneObservation::ObserveCandidate) fills these structures from guarded
// reads on the inspection frame, resolves every reference on that frame and keeps none of them.
//
// The question the inspection instruments (M6-D research record, hypothesis B1): is the second
// entity a complete but unspawned hero, or a partially initialized, misplaced or incomplete one?
// It reads, for the candidate and for the local hero as its control, the STORED spatial fields
// (the cached object-to-world translation with its dirty flag, the parent-relative translation and
// rotation, the parent entity's authored id, the presence of the transform-tree links, the
// visibility bits), the PRESENCE of the hero's simulation members (a non-null pointer or reference,
// and for entity references whether their interface resolved) and the m_bActive property through
// the SDK's property mechanism; the spawn entity's stored transform and its four authored
// properties through the same mechanism; and the four registry slots' entity identity on the same
// frame. It establishes presence and some validity evidence; it never claims that a component is
// initialized or simulating, that an entity is placed, visible or playable, or that a spawn ran.
//
// The 48-entry cap and the 4,096-byte line bound are the protocol's. The inspection fits them by
// packing groups of values that share one availability condition into one typed text entry with a
// documented, fixed grammar (below), never by widening a bound. Every packed group keeps its own
// status: a group whose engine read did not happen is unavailable with the reason, never a
// default. The grammar (each a text value of at most 128 bytes):
//   <prefix>_world         "x,y,z"                   m_mTransform.Trans as stored (7 significant digits)
//   <prefix>_local         "tx,ty,tz|qx,qy,qz,qw"    m_vObjectToParentTranslation | m_vObjectToParentRotation
//   <prefix>_parent        "<16 hex>" | "none"       m_eidParent's authored entity id, or none
//   <prefix>_spatial_flags "visible=B,parent_hidden=B,force_visible=B,dynamic=B,scene_child=B,dirty=B,room=N,tparent=B,tchildren=B"
//   <prefix>_members       "character=M,animator=M,morpheme=M,ragdoll=M,health=M,kit=M,camera=M,input=M"
//   <prefix>_controllers   "guide=M,ik=M,cheat=M,interaction=M,actor_tag=M"
//   <prefix>_outfit        "<kit guid>|none,charset,variation"
//   spawn_select           "default=B,preferred=B"
//   registry_slots         "0:T,1:T,2:T,3:T"  T = empty | local | candidate | other | inconclusive, with "-nohero"
//                          appended when the slot's reference did not resolve a ZHitman5 interface
//   targets                "<brick>,<entity>,<spawn>" the three authored targets the inspection used
// where B is 0|1, N an integer, and M a member state: 0 = null, 1 = present (a non-null pointer or
// entity reference), 2 = valid (an entity reference whose interface also resolved). "Present" is
// never "initialized" or "simulating".
namespace CandidateObservation
{
    using OutfitObservation::Field;
    using OutfitObservation::Entries;
    using FieldStatus = CommandProtocol::FieldStatus;

    // The default authored targets (the historical hitmen.brick, Agent48 and Agent48Spawn) as the
    // text the inspection echoes when the command names none. The Glacier-facing reader derives the
    // brick's runtime id from the resource path itself; these are documentation and the test/probe
    // fallback, never a pointer.
    constexpr const char* k_DefaultBrick = "007c57a6ea581f8c";
    constexpr const char* k_DefaultEntity = "feede715906f747f";
    constexpr const char* k_DefaultSpawn = "feed755acdd56780";
    constexpr uint64_t k_DefaultSpawnId = 0xfeed755acdd56780ull;

    constexpr size_t k_SlotCount = PlayerObservation::k_SlotCount;
    constexpr size_t k_MaxClassNameBytes = 64;   // the spawn's class name text bound
    constexpr size_t k_MaxInterfacesScanned = 64; // bound of the entity-type interface-table walk

    // ---- spatial ---------------------------------------------------------------------------------

    struct Vec3
    {
        double x = 0, y = 0, z = 0;
    };

    struct Quat
    {
        double x = 0, y = 0, z = 0, w = 0;
    };

    struct LocalTransform
    {
        Vec3 translation; // m_vObjectToParentTranslation
        Quat rotation;    // m_vObjectToParentRotation
    };

    struct SpatialFlags
    {
        bool visible = false;            // m_bVisible
        bool parent_hidden = false;      // m_bParentHidden
        bool force_visible = false;      // m_bForceVisible
        bool dynamic = false;            // m_bIsDynamic
        bool scene_child = false;        // m_bSceneChild
        bool dirty = false;              // m_bWorldTransformDirty: the stored world translation may be stale
        int64_t room_behaviour = 0;      // m_eRoomBehaviour
        bool transform_parent = false;   // m_pTransformParent non-null (never dereferenced)
        bool transform_children = false; // m_pTransformChildren non-null (never dereferenced)
    };

    struct SpatialReads
    {
        Field<Vec3> world;           // the cached object-to-world translation as stored (never through the refreshing getter)
        Field<LocalTransform> local; // the parent-relative transform as stored
        Field<std::string> parent;   // the parent entity's authored id (16 hex) or "none"
        Field<SpatialFlags> flags;
    };

    // ---- hero completeness -----------------------------------------------------------------------

    // A member's state: null; present (non-null pointer or entity reference); valid (an entity
    // reference whose interface resolved on this frame). Presence is never initialization.
    enum class Member : int
    {
        Null = 0,
        Present = 1,
        Valid = 2,
    };

    struct HeroMembers
    {
        Member character = Member::Null;        // m_pCharacter (entity reference)
        Member animator = Member::Null;         // m_Animator (entity reference; 0/1 only)
        Member morpheme = Member::Null;         // m_MorphemeEntityID (entity reference; 0/1 only)
        Member ragdoll = Member::Null;          // m_rRagdollEntity (entity reference)
        Member health = Member::Null;           // m_pHealth (pointer; 0/1 only)
        Member outfit_kit = Member::Null;       // m_rOutfitKit (entity reference)
        Member main_camera = Member::Null;      // m_rMainCamera (entity reference)
        Member input_processor = Member::Null;  // m_pCharacterInputProcessor (pointer; 0/1 only)
    };

    struct HeroControllers
    {
        Member guide = Member::Null;       // m_pGuideController
        Member ik = Member::Null;          // m_pIKController
        Member cheat = Member::Null;       // m_pCheatController
        Member interaction = Member::Null; // m_pInteractionController
        Member actor_tag = Member::Null;   // m_pActorTagController
    };

    struct OutfitTuple
    {
        std::string kit_id; // the current kit's repository id (dashed lowercase) or "none"
        int64_t charset = 0;
        int64_t variation = 0;
    };

    // One hero's state beyond its identity (the identity lives in CandidateReads).
    struct HeroReads
    {
        SpatialReads spatial;
        Field<HeroMembers> members;
        Field<HeroControllers> controllers;
        Field<OutfitTuple> outfit;
        Field<bool> active; // m_bActive through the property mechanism; never a proven spawn state
    };

    // ---- spawn -----------------------------------------------------------------------------------

    struct SpawnReads
    {
        PlayerObservation::LookupFacts lookup; // the spawn's own factory lookup (same bounds as M6-B1); same_entity: the spawn is the candidate entity (expected false; not reported)
        Field<std::string> class_name;         // "ZHeroSpawn" when the entity exposes that interface; otherwise the first interface type name
        Field<Vec3> world;                     // the spawn entity's stored transform translation
        Field<bool> dirty;                     // its dirty flag
        Field<int64_t> player_id;              // m_nPlayerId (property mechanism)
        Field<bool> default_spawn;             // m_bDefaultHeroSpawn
        Field<bool> preferred_spawn;           // m_bPreferredHeroSpawn
        Field<std::string> hitman_ref;         // m_rHitmanCharacter's authored entity id (16 hex) or "none"
        Field<bool> ref_is_candidate;          // that reference is the candidate entity on this frame (authored relationship; not evidence the spawn ran)
    };

    // ---- registry membership by identity -------------------------------------------------------

    enum class SlotMatch
    {
        Empty,        // m_HitmanEntity null
        Local,        // the slot's entity is the local hero (registry slot 0's reference)
        Candidate,    // the slot's entity is the candidate resolved on this frame
        Other,        // a resolved entity that is neither
        Inconclusive, // the candidate was not resolved on this frame, so the comparison could not be made
    };

    struct SlotIdentity
    {
        SlotMatch match = SlotMatch::Empty;
        bool hero_resolved = false; // the ZHitman5 interface resolved from the slot's reference (meaningful when not empty)
    };

    // What the frame established about the candidate before the slots are compared: resolved (an
    // identity to compare with), known absent (absent on a COMPLETE scan: no slot can be it), or
    // inconclusive (present but unresolved, or presence not established).
    enum class CandidateKnowledge
    {
        Resolved,
        KnownAbsent,
        Inconclusive,
    };

    CandidateKnowledge KnowledgeOf(bool p_Resolved, const Field<bool>& p_BrickPresent);

    // The pure classification of one slot from this frame's identity comparisons (the Glacier-facing
    // reader supplies the comparisons; the rule lives here so the tests exercise exactly it).
    SlotMatch ClassifySlot(bool p_Present, bool p_IsLocal, bool p_IsCandidate, CandidateKnowledge p_Knowledge);

    // ---- the read set ----------------------------------------------------------------------------

    struct CandidateReads
    {
        Field<std::string> generation_fresh; // registry slot 0's session id read twice on this frame
        Field<std::string> hero_entity_id;   // the local hero's authored id
        Field<int64_t> hero_flags;           // ZEntityImpl::m_nEntityFlags of the local hero
        PlayerObservation::BrickReads bricks;

        std::string brick_id;  // the targets as used (16 hex each)
        std::string entity_id;
        std::string spawn_id;

        Field<bool> brick_present;             // CandidatePresence: false only from a complete scan
        PlayerObservation::LookupFacts lookup; // the candidate's factory lookup (same_entity: it resolved to the local hero)
        Field<bool> hitman5;                   // the ZHitman5 interface resolved on the candidate
        Field<int64_t> cand_flags;             // the candidate's entity flags
        Field<bool> id_match;                  // the resolved entity's type carries the queried authored id
        Field<std::string> cand_brick_owner;   // the candidate's owning entity named as a brick root / "scene"
        Field<std::string> registry_slots;     // the four slots' identity classes (EncodeSlots)

        HeroReads hero;      // the local hero (the control)
        HeroReads candidate; // the candidate
        SpawnReads spawn;
        uint32_t thread_id = 0;
    };

    struct CandidateQuery
    {
        std::optional<std::string> brick;     // 16 lowercase hex; absent = the historical hitmen.brick
        std::optional<std::string> entity_id; // 16 lowercase hex; absent = the historical Agent48 id
    };

    // ---- pure helpers ----------------------------------------------------------------------------

    const char* MemberText(Member p_Member);     // "0" | "1" | "2"
    const char* SlotMatchText(SlotMatch p_Match); // "empty" | "local" | ...

    std::string Number(double p_Value);          // 7 significant digits, shortest form
    std::string EncodeVec3(const Vec3& p_Vec);   // "x,y,z"
    std::string EncodeLocal(const LocalTransform& p_Local);
    std::string EncodeSpatialFlags(const SpatialFlags& p_Flags);
    std::string EncodeMembers(const HeroMembers& p_Members);
    std::string EncodeControllers(const HeroControllers& p_Controllers);
    std::string EncodeOutfit(const OutfitTuple& p_Outfit);
    std::string EncodeSelect(bool p_Default, bool p_Preferred);
    std::string EncodeSlots(const SlotIdentity (&p_Slots)[k_SlotCount]);
    std::string EncodeTargets(std::string_view p_Brick, std::string_view p_Entity, std::string_view p_Spawn);

    // The deterministic read-back: the identity block, the registry verdict, the candidate's state,
    // the spawn, then the local hero's state (the control) last, so that the protocol's own
    // reduction — reasons first, then plain entries from the END — removes the control before the
    // candidate and never the identity. 43 entries; the scheduler adds four plain frame values.
    void AppendCandidate(Entries& p_Out, const CandidateReads& p_Reads);

    // Every field marked with one status and reason (no inspector, faults).
    void MarkSpatial(SpatialReads& p_Spatial, FieldStatus p_Status, const char* p_Reason);
    void MarkHero(HeroReads& p_Hero, FieldStatus p_Status, const char* p_Reason);
    void MarkSpawn(SpawnReads& p_Spawn, FieldStatus p_Status, const char* p_Reason);
    CandidateReads AllCandidate(FieldStatus p_Status, const char* p_Reason, const CandidateQuery& p_Query);
}
