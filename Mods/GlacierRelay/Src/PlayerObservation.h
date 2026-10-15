#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "CommandProtocol.h"
#include "OutfitObservation.h"

// What the two M6-B1 read-only inspections can report (relay.inspect.players v1 and
// relay.inspect.entity v1), as plain owned values with an explicit per-field status, exactly as the
// M5 C-1 outfit inspection does (OutfitObservation). Nothing here is an engine type, a pointer, an
// entity reference, a resource handle or an address: the Glacier-facing reader
// (SceneObservation::ObservePlayers / ObserveEntity) fills these structures from guarded reads on
// the inspection frame, resolves every reference on that frame and keeps none of them, and the
// scheduler encodes each field as {"status": ..., "value": ..., "reason": ...}. A missing value is
// never a zero, a default or a guess.
//
// Identity is an evidence tuple, never a pointer: an owning brick (a runtime resource id as 16 hex
// digits), an authored entity id (16 hex digits), the mission generation and the frame the reads
// were made on, and the entity's validity flags. A pointer compared across frames is reported as an
// observation of equality only; it is never the identity of a player or an entity.
//
// The pure helpers at the end (word classification, hex rendering, the round-trip classification,
// the encoding) are the one place the rules live, so the tests exercise exactly what the DLL applies
// with fake values and never with an engine pointer.
namespace PlayerObservation
{
    using OutfitObservation::Field;
    using OutfitObservation::Entries;
    using FieldStatus = CommandProtocol::FieldStatus;

    constexpr size_t k_SlotCount = 4;          // the registry's four declared inline player slots
    constexpr size_t k_MaxBricksScanned = 64;  // bound of the loaded-brick traversal
    constexpr size_t k_IdsPerField = 7;        // 7 × 16 hex + 6 commas = 118 bytes, within the 128-byte text bound
    constexpr size_t k_MaxIdFields = 3;        // at most 21 brick ids reported; the count says how many exist
    constexpr size_t k_MaxBrickIds = k_IdsPerField * k_MaxIdFields;

    // Mirrors ZEntityImpl::EEntityFlags for the two flags the inspection reports (the Glacier-facing
    // reader static_asserts the SDK's values against these).
    constexpr int64_t k_FlagActivated = 4;
    constexpr int64_t k_FlagPredeleted = 8;

    // M6-B1 hardening, correction A: a factory's sub-entity count is used only when it is valid —
    // non-negative and within a plausibility bound (a documented bound against a garbage read, not an
    // engine limit; a larger count is reported as read and refused as invalid) — and a sub-entity is
    // accessed only at an index inside [0, count). The check is a typed observation beside the index.
    constexpr int64_t k_MaxSubEntityCount = 1 << 20;

    // ---- relay.inspect.players v1 ----------------------------------------------------------------

    struct SlotReads
    {
        Field<int64_t> player_id;        // m_nPlayerId
        Field<bool> local_flag;          // m_bLocalPlayer as read: NOT a discriminator (experiment 4: true on all four slots)
        Field<bool> connected_flag;      // m_bConnectedToMultiplayer
        Field<bool> net_player;          // m_pNetPlayer non-null
        Field<bool> replica;             // m_pRakNetReplica non-null
        Field<bool> entity_present;      // m_HitmanEntity non-null — a reference, never a living hero
        Field<bool> hero_resolved;       // the ZHitman5 interface resolved from that reference on this frame
        Field<int64_t> flags_a0;         // the word at controller offset 0xA0 (0 on the local slot, -1 elsewhere in experiment 4)
        Field<std::string> session_id;   // the slot's session id string, plausibility-checked
        Field<std::string> outfit_id;    // the controller's outfit guid (dashed lowercase)
        Field<std::string> character_id; // the controller's selected character guid (dashed lowercase)
    };

    struct RegistryReads
    {
        Field<bool> registry_available;  // Globals::PlayerRegistry non-null
        Field<std::string> words;        // the three words at 0x390 classified: "slot0,0,0x4000000000000101"
        SlotReads slots[k_SlotCount];
        Field<std::string> hero_kit_id;  // slot 0 through M5's local-hero path: the entity's current outfit kit id
        Field<bool> slot0_matches_m5;    // the registry's slot-0 outfit guid equals that kit id on this frame
        uint32_t thread_id = 0;
    };

    // ---- relay.inspect.entity v1 -----------------------------------------------------------------

    struct EntityIdentity
    {
        Field<bool> resolved;              // reference non-null and the ZHitman5 interface resolved
        Field<std::string> entity_id;      // the authored entity id (16 hex)
        Field<int64_t> entity_flags;       // ZEntityImpl::m_nEntityFlags
        Field<bool> activated;             // flags & ENTITYFLAG_ACTIVATED
        Field<bool> predeleted;            // flags & ENTITYFLAG_PREDELETED
        Field<std::string> owning_brick;   // "<16 hex>" when the owning entity is a loaded brick's root; "scene" when it is the scene root
        Field<std::string> character_id;   // ZHitman5::m_CharacterId (dashed lowercase)
        Field<std::string> initial_outfit_id; // ZHitman5::m_InitialOutfitId (dashed lowercase)
        Field<bool> invincible;            // ZHitman5::m_bIsInvincible
        Field<double> x, y, z;             // ZSpatialEntity::m_mTransform.Trans as stored — never through a refreshing getter
        Field<bool> transform_dirty;       // ZSpatialEntity::m_bWorldTransformDirty: the stored translation may be stale
        Field<int64_t> room_behaviour;     // ZSpatialEntity::m_eRoomBehaviour
    };

    struct BrickReads
    {
        Field<int64_t> count;            // m_aLoadedBricks.size()
        std::vector<std::string> ids;    // the first k_MaxBrickIds runtime resource ids (16 hex each)
        Field<bool> truncated;           // more bricks exist than were reported
    };

    // The facts of one blueprint-factory lookup as plain values. For the local hero's round trip the
    // reference is the hero itself (same_entity: the lookup returned the hero); for the candidate the
    // reference is still the local hero (same_entity false means the candidate is a different entity).
    struct LookupFacts
    {
        Field<bool> owner_found;         // an owning entity (the hero's) or the named brick root (the candidate's) exists
        Field<bool> factory_available;   // GetBlueprintFactory() on it is non-null
        Field<bool> factory_template;    // that factory's vtable is the template entity blueprint factory's
        Field<int64_t> subentity_count;  // GetSubEntitiesCount(), as read (valid or not)
        Field<int64_t> index;            // GetSubEntityIndex(entity id), as read; unavailable when the count was invalid (not looked up)
        Field<std::string> index_check;  // correction A: "in_range" | "negative" | "out_of_range" | "count_invalid" (IndexCheckText)
        Field<bool> resolved;            // GetSubEntity(owner, index) returned a non-null entity — called only when index_check is in_range
        Field<bool> same_entity;         // that entity is the reference entity (pointer equality on this frame; an observation, not an identity)
    };

    // Correction A: the bounds check of one lookup as a closed vocabulary. A negative index is the
    // factory's own "not a sub-entity" answer (the SDK's convention is -1; any negative value is
    // reported as read); a non-negative index at or beyond the count, or an invalid count, is an
    // inconsistent factory answer and refuses the sub-entity access. The two are never conflated.
    enum class IndexCheck
    {
        InRange,      // 0 <= index < count: the one case in which GetSubEntity may be called
        Negative,     // index < 0: not a sub-entity of that factory
        OutOfRange,   // count valid, index >= count
        CountInvalid, // count < 0 or count > k_MaxSubEntityCount: the index is not looked up
    };

    const char* IndexCheckText(IndexCheck p_Check);
    bool SubEntityCountValid(int64_t p_Count);
    IndexCheck CheckSubEntityIndex(int64_t p_Count, int64_t p_Index);

    // The six outcomes the authorization distinguishes (M6-B1 section 5).
    enum class RoundTrip
    {
        SameEntity,            // 1. a valid lookup returned the original entity
        DifferentEntity,       // 2. a valid lookup returned a different entity: the one specific failure of the lookup contract
        OwnerUnresolved,       // 3. the owning entity / brick could not be established
        HeroNotRepresentable,  // 4. the hero's id is not a sub-entity of that factory (a negative index)
        FactoryUnavailable,    // 5. a required factory or interface was unavailable (null, or not a template factory)
        Unsupported,           // 6. unsupported or inconclusive (a read failed, the count or index was out of bounds — index_check says which — or a valid index resolved to nothing)
    };

    const char* RoundTripText(RoundTrip p_Outcome);
    RoundTrip ClassifyRoundTrip(const LookupFacts& p_Facts);

    struct CandidateReads
    {
        std::string brick_id;            // the query's brick (16 hex; the default when the command named none)
        std::string entity_id;           // the query's authored entity id (16 hex; the default when none)
        Field<bool> brick_present;       // the brick id is in the loaded-brick list
        LookupFacts lookup;              // same_entity: the candidate resolved to the local hero's own entity
        EntityIdentity identity;         // the candidate's identity when it resolved
        Field<bool> hitman5_resolved;
        Field<bool> spatial_resolved;
    };

    struct EntityQuery
    {
        std::optional<std::string> brick;     // 16 lowercase hex; absent = the historical hitmen.brick
        std::optional<std::string> entity_id; // 16 lowercase hex; absent = the historical Agent48 id
    };

    struct EntityReads
    {
        BrickReads bricks;
        EntityIdentity hero;             // the local hero from registry slot 0 (M5's path, unchanged)
        LookupFacts round_trip;          // the hero looked up through its owning entity's factory
        RoundTrip round_trip_outcome = RoundTrip::Unsupported;
        std::optional<CandidateReads> candidate;
        uint32_t thread_id = 0;
    };

    // ---- pure helpers ----------------------------------------------------------------------------

    // The 64-bit value as 16 lowercase hex digits.
    std::string Hex16(uint64_t p_Value);

    // Exactly 16 lowercase hex digits.
    bool ValidHex16(std::string_view p_Text);
    bool ParseHex16(std::string_view p_Text, uint64_t& p_Out);

    // One registry word at 0x390 classified against the inline slots: "slot<k>" when it is the
    // address of slot k, "0" when zero, otherwise its hex value (a value, never an identity).
    std::string ClassifyWord(uint64_t p_Word, uint64_t p_SlotsBase, uint64_t p_SlotSize, size_t p_SlotCount);

    // The brick ids grouped k_IdsPerField per text field (at most k_MaxIdFields fields).
    std::vector<std::string> GroupIds(const std::vector<std::string>& p_Ids);

    // Encoding: deterministic field order; the two inspections' observed maps. Plain scheduler
    // values (frame, thread) are appended by the scheduler itself.
    void Append(Entries& p_Out, const char* p_Name, const Field<double>& p_Field);
    void AppendRegistry(Entries& p_Out, const RegistryReads& p_Reads);
    void AppendEntity(Entries& p_Out, const EntityReads& p_Reads);

    // Every field of a read set (or of one identity / lookup) marked with one status and reason
    // (for "no inspector", faults and references that could not be followed).
    void MarkIdentity(EntityIdentity& p_Identity, FieldStatus p_Status, const char* p_Reason);
    void MarkLookup(LookupFacts& p_Facts, FieldStatus p_Status, const char* p_Reason);
    RegistryReads AllRegistry(FieldStatus p_Status, const char* p_Reason);
    EntityReads AllEntity(FieldStatus p_Status, const char* p_Reason, const std::optional<EntityQuery>& p_Query);
}
