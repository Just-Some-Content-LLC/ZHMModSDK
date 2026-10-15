#include "SceneObservation.h"

#include <Windows.h>

#include <array>
#include <cstring>

#include <algorithm>
#include <utility>
#include <vector>

#include "Functions.h"
#include "Globals.h"
#include "Glacier/CompileReflection.h"
#include "Glacier/EntityFactory.h"
#include "Glacier/Enums.h"
#include "Glacier/ZApplicationEngineWin32.h"
#include "Glacier/ZContentKitManager.h"
#include "Glacier/ZHitman5.h"
#include "Glacier/ZModule.h"
#include "Glacier/ZOutfit.h"
#include "Glacier/ZPlayerRegistry.h"
#include "Glacier/ZScene.h"
#include "Glacier/ZSpatialEntity.h"

#include "RepositoryId.h"

SceneState SceneObservation::ObserveScene()
{
    SceneState s_State;

    const auto* s_Context = Globals::Hitman5Module ? Globals::Hitman5Module->m_pEntitySceneContext : nullptr;
    const auto* s_Engine = Globals::ApplicationEngineWin32 ? *Globals::ApplicationEngineWin32 : nullptr;

    if (!s_Context || !s_Engine)
        return s_State;

    const auto& s_Parameters = s_Context->m_SceneInitParameters;

    s_State.available = true;
    s_State.scene_resource = std::string(s_Parameters.m_SceneResource.ToStringView());
    s_State.scene_type = std::string(s_Parameters.m_Type.ToStringView());
    s_State.codename_hint = std::string(s_Parameters.m_CodeNameHint.ToStringView());
    s_State.loading_stage = static_cast<int32_t>(s_Context->m_LoadingStage);
    s_State.scene_loaded = s_Context->m_pScene && s_Engine->m_bSceneLoaded;

    return s_State;
}

std::optional<std::string> SceneObservation::ObserveGameSessionId()
{
    const auto* s_Registry = Globals::PlayerRegistry;

    if (!s_Registry)
        return std::nullopt;

    // Inline slot 0 only. Its layout held in every dump of experiment 4; the SDK's array model of the
    // registry at 0x390 did not, and is not used here.
    const auto& s_SessionId = s_Registry->m_aPlayerData[0].m_Controller.s_sSessionId;

    if (s_SessionId.size() == 0 || s_SessionId.size() > 256 || !s_SessionId.c_str())
        return std::nullopt;

    return std::string(s_SessionId.ToStringView());
}

// ---- M5 C-1: the frame-local outfit inspection ------------------------------------------------
namespace
{
    using OutfitObservation::Field;
    using OutfitObservation::FieldStatus;
    using OutfitObservation::HeroOutfit;
    using OutfitObservation::KitResolution;

    constexpr size_t k_HeroACharacterIndex = static_cast<size_t>(ECharSetCharacterType::ECSCT_HeroA);
    constexpr size_t k_MaxKitsScanned = 4096; // the bounded scan of the kit map (hundreds in practice)

    static_assert(sizeof(ZRepositoryID) == RepositoryId::k_Bytes, "ZRepositoryID is expected to be 16 bytes");
    static_assert(!std::is_polymorphic_v<ZRepositoryID>, "ZRepositoryID is expected to have no vtable");

    // The same rendering the telemetry intake uses for disguise ids (M2 design section 35): the
    // engine value's bytes copied out, never the engine's own string functions.
    std::string RenderId(const ZRepositoryID& p_Id)
    {
        std::array<uint8_t, RepositoryId::k_Bytes> s_Bytes{};
        std::memcpy(s_Bytes.data(), &p_Id, s_Bytes.size());
        return RepositoryId::FromLittleEndianBytes(s_Bytes).ToDashedLowercase();
    }

    // HeroA variation count of one charset of a kit: every indexed read bounds-checked first.
    // Returns the count, or the reason it could not be read.
    Field<int64_t> HeroAVariations(const ZGlobalOutfitKit& p_Kit, int64_t p_Charset)
    {
        if (p_Charset < 0 || static_cast<size_t>(p_Charset) >= p_Kit.m_aCharSets.size())
            return Field<int64_t>::Of(FieldStatus::Unavailable, "charset index out of bounds");

        const auto& s_CollectionRef = p_Kit.m_aCharSets[static_cast<size_t>(p_Charset)];
        if (!s_CollectionRef || !s_CollectionRef.m_pInterfaceRef)
            return Field<int64_t>::Of(FieldStatus::Unavailable, "charset collection null");

        const auto& s_Characters = s_CollectionRef.m_pInterfaceRef->m_aCharacters;
        if (s_Characters.size() <= k_HeroACharacterIndex)
            return Field<int64_t>::Of(FieldStatus::Unavailable, "no HeroA character type in charset");

        const auto& s_HeroARef = s_Characters[k_HeroACharacterIndex];
        if (!s_HeroARef || !s_HeroARef.m_pInterfaceRef)
            return Field<int64_t>::Of(FieldStatus::Unavailable, "HeroA character type null");

        return Field<int64_t>::Observed(static_cast<int64_t>(s_HeroARef.m_pInterfaceRef->m_aVariations.size()));
    }

    // The kit's shape as plain values: one bounds-checked HeroA read per charset (at most
    // k_MaxCharsetsRead; a kit with more reports the rest unread). The validity rules themselves
    // live in OutfitObservation::Describe*, shared with the tests.
    constexpr size_t k_MaxCharsetsRead = 16;

    OutfitObservation::KitShape ShapeOf(const ZGlobalOutfitKit& p_Kit)
    {
        OutfitObservation::KitShape s_Shape;
        s_Shape.id = RenderId(p_Kit.m_sId);
        s_Shape.is_hitman_suit = p_Kit.m_bIsHitmanSuit;

        const size_t s_Charsets = p_Kit.m_aCharSets.size();
        for (size_t i = 0; i < s_Charsets && i < k_MaxCharsetsRead; ++i)
            s_Shape.heroa_variations.push_back(HeroAVariations(p_Kit, static_cast<int64_t>(i)));
        for (size_t i = k_MaxCharsetsRead; i < s_Charsets; ++i)
            s_Shape.heroa_variations.push_back(Field<int64_t>::Of(FieldStatus::Unavailable, "charset beyond the read bound"));

        return s_Shape;
    }

    void ReadHeroOutfit(const ZHitman5& p_Hero, HeroOutfit& p_Out)
    {
        const auto& s_KitRef = p_Hero.m_rOutfitKit;

        if (!s_KitRef || !s_KitRef.m_pInterfaceRef)
        {
            p_Out.outfit_id = Field<std::string>::Of(FieldStatus::Unavailable, "outfit kit reference null");
            p_Out.heroa_valid = Field<bool>::Of(FieldStatus::Unavailable, "outfit kit reference null");
            return;
        }

        OutfitObservation::DescribeHeroOutfit(ShapeOf(*s_KitRef.m_pInterfaceRef), p_Hero.m_nOutfitCharset, p_Hero.m_nOutfitVariation, p_Out);
    }

    void DescribeKit(const ZGlobalOutfitKit& p_Kit, KitResolution& p_Out)
    {
        OutfitObservation::DescribeCandidate(ShapeOf(p_Kit), p_Out);
    }

    std::optional<std::string> ReadSessionIdOnce(const ZPlayerRegistry& p_Registry)
    {
        const auto& s_SessionId = p_Registry.m_aPlayerData[0].m_Controller.s_sSessionId;

        if (s_SessionId.size() == 0 || s_SessionId.size() > 256 || !s_SessionId.c_str())
            return std::nullopt;

        return std::string(s_SessionId.ToStringView());
    }
}

OutfitObservation::EngineReads SceneObservation::ObserveOutfit(const std::optional<std::string>& p_CandidateId, OutfitHandles* p_Handles)
{
    if (p_Handles)
        *p_Handles = OutfitHandles{};

    OutfitObservation::EngineReads s_Reads;
    s_Reads.thread_id = GetCurrentThreadId();

    // 1. The registry: the session id read twice on this frame; the local hero from slot 0 (the
    //    inline slot the adapter has read since M2; not the SDK's TArray model of the registry).
    const ZPlayerRegistry* s_Registry = Globals::PlayerRegistry;

    if (!s_Registry)
    {
        s_Reads.generation_fresh = Field<std::string>::Of(FieldStatus::Unavailable, "player registry global null");
        s_Reads.hero.outfit_id = Field<std::string>::Of(FieldStatus::Unavailable, "player registry global null");
        s_Reads.hero.heroa_valid = Field<bool>::Of(FieldStatus::Unavailable, "player registry global null");
    }
    else
    {
        const auto s_First = ReadSessionIdOnce(*s_Registry);
        const auto s_Second = ReadSessionIdOnce(*s_Registry);

        if (!s_First || !s_Second)
            s_Reads.generation_fresh = Field<std::string>::Of(FieldStatus::Unavailable, "registry session id empty or implausible");
        else if (*s_First != *s_Second)
            s_Reads.generation_fresh = Field<std::string>::Of(FieldStatus::Inconsistent, "registry session id reads differ");
        else
            s_Reads.generation_fresh = Field<std::string>::Observed(*s_First);

        const ZEntityRef s_HeroRef = s_Registry->m_aPlayerData[0].m_Controller.m_HitmanEntity;

        if (!s_HeroRef)
        {
            s_Reads.hero.outfit_id = Field<std::string>::Of(FieldStatus::Unavailable, "hero entity reference null");
            s_Reads.hero.heroa_valid = Field<bool>::Of(FieldStatus::Unavailable, "hero entity reference null");
        }
        else
        {
            const TEntityRef<ZHitman5> s_Hero(s_HeroRef);

            if (!s_Hero || !s_Hero.m_pInterfaceRef)
            {
                s_Reads.hero.outfit_id = Field<std::string>::Of(FieldStatus::Unavailable, "hero interface not resolved");
                s_Reads.hero.heroa_valid = Field<bool>::Of(FieldStatus::Unavailable, "hero interface not resolved");
            }
            else
            {
                s_Reads.hero.player_resolved = true;
                ReadHeroOutfit(*s_Hero.m_pInterfaceRef, s_Reads.hero);
                if (p_Handles)
                    p_Handles->hero = s_Hero;
            }
        }
    }

    // 2. The engine's kit map and the optional candidate, by a bounded byte comparison of the
    //    repository id (the map's own comparator is not relied on).
    const ZContentKitManager* s_Kits = Globals::ContentKitManager;
    s_Reads.kit_map_available = Field<bool>::Observed(s_Kits != nullptr);

    if (p_CandidateId)
    {
        KitResolution s_Resolution;
        RepositoryId s_Wanted;

        if (!s_Kits)
        {
            s_Resolution.found = Field<bool>::Of(FieldStatus::Unavailable, "content kit manager global null");
        }
        else if (!RepositoryId::FromDashedLowercase(*p_CandidateId, s_Wanted))
        {
            s_Resolution.found = Field<bool>::Of(FieldStatus::Unavailable, "candidate id not parseable");
        }
        else
        {
            const auto s_WantedBytes = s_Wanted.ToLittleEndianBytes();
            size_t s_Scanned = 0;
            bool s_Found = false;
            bool s_Truncated = false;

            for (const auto& s_Pair : s_Kits->m_repositoryGlobalOutfitKits)
            {
                if (++s_Scanned > k_MaxKitsScanned)
                {
                    s_Truncated = true;
                    break;
                }

                if (std::memcmp(&s_Pair.first, s_WantedBytes.data(), s_WantedBytes.size()) != 0)
                    continue;

                s_Found = true;

                if (!s_Pair.second || !s_Pair.second.m_pInterfaceRef)
                {
                    s_Resolution.found = Field<bool>::Of(FieldStatus::Unavailable, "kit entry reference null");
                }
                else
                {
                    DescribeKit(*s_Pair.second.m_pInterfaceRef, s_Resolution);
                    if (p_Handles)
                        p_Handles->kit = s_Pair.second;
                }
                break;
            }

            if (!s_Found)
                s_Resolution.found = s_Truncated ? Field<bool>::Of(FieldStatus::Unavailable, "kit map scan bound reached") : Field<bool>::Observed(false);

            s_Resolution.kits_scanned = Field<int64_t>::Observed(static_cast<int64_t>(s_Scanned));
        }

        s_Reads.candidate = s_Resolution;
    }

    // 3. The setter's availability on this build: the resolved address exists or it does not.
    s_Reads.set_outfit_available = Field<bool>::Observed(Functions::ZHitman5_SetOutfit != nullptr && Functions::ZHitman5_SetOutfit->Exists());

    return s_Reads;
}

// ---- M6-B1: the player-registry and entity inspections ------------------------------------------
namespace
{
    using PlayerObservation::EntityIdentity;
    using PlayerObservation::EntityReads;
    using PlayerObservation::LookupFacts;
    using PlayerObservation::RegistryReads;
    using PlayerObservation::SlotReads;
    using OutfitObservation::Field;
    using OutfitObservation::FieldStatus;

    static_assert(static_cast<int64_t>(ZEntityImpl::EEntityFlags::ENTITYFLAG_ACTIVATED) == PlayerObservation::k_FlagActivated, "ENTITYFLAG_ACTIVATED is expected to be 4");
    static_assert(static_cast<int64_t>(ZEntityImpl::EEntityFlags::ENTITYFLAG_PREDELETED) == PlayerObservation::k_FlagPredeleted, "ENTITYFLAG_PREDELETED is expected to be 8");
    static_assert(sizeof(ZGuid) == RepositoryId::k_Bytes, "ZGuid is expected to be 16 bytes");
    static_assert(sizeof(ZPlayerRegistry::m_PlayerData) >= 3 * sizeof(uint64_t), "the three words at 0x390 lie inside the declared registry object");
    static_assert(offsetof(ZPlayerRegistry, m_PlayerData) == 0x390, "the registry words are read at 0x390");

    // The historical candidate: hitmen.brick and its authored hero (Agent48). Content, not a pointer.
    const ZRuntimeResourceID k_HitmenBrick = ResId<"[assembly:/_sdk/hitmen.brick].pc_entitytype">;
    constexpr uint64_t k_HitmenAgent48 = 0xfeede715906f747full;

    // Any 16-byte engine guid rendered the way the telemetry intake renders disguise ids (the
    // bytes copied out; never the engine's own string functions, which allocate).
    std::string RenderGuid(const ZGuid& p_Guid)
    {
        std::array<uint8_t, RepositoryId::k_Bytes> s_Bytes{};
        std::memcpy(s_Bytes.data(), &p_Guid, s_Bytes.size());
        return RepositoryId::FromLittleEndianBytes(s_Bytes).ToDashedLowercase();
    }

    Field<std::string> ReadSessionField(const ZString& p_Text)
    {
        if (p_Text.size() == 0)
            return Field<std::string>::Of(FieldStatus::Unavailable, "session id empty");
        if (p_Text.size() > 256 || !p_Text.c_str())
            return Field<std::string>::Of(FieldStatus::Unavailable, "session id implausible");
        return Field<std::string>::Observed(std::string(p_Text.ToStringView()));
    }

    // A bounded copy of the loaded-brick list's ids and root references, valid for this frame only.
    using BrickList = std::vector<std::pair<uint64_t, ZEntityRef>>;

    // The list is scanned in a bounded prefix (k_MaxBricksScanned) for the lookups, and the first
    // k_MaxBrickIds ids are reported for display; the two bounds are reported separately (correction
    // C): "scanned" says how far the lookup scan went, "truncated" says the display is partial.
    void ReadBricks(const ZEntitySceneContext* p_Context, PlayerObservation::BrickReads& p_Out, BrickList& p_List)
    {
        if (!p_Context)
        {
            p_Out.count = Field<int64_t>::Of(FieldStatus::Unavailable, "scene context null");
            p_Out.truncated = Field<bool>::Of(FieldStatus::Unavailable, "scene context null");
            p_Out.scanned = Field<int64_t>::Of(FieldStatus::Unavailable, "scene context null");
            return;
        }

        const auto& s_Bricks = p_Context->m_aLoadedBricks;
        const size_t s_Count = s_Bricks.size();
        p_Out.count = Field<int64_t>::Observed(static_cast<int64_t>(s_Count));

        const size_t s_Scanned = std::min(s_Count, PlayerObservation::k_MaxBricksScanned);
        for (size_t i = 0; i < s_Scanned; ++i)
        {
            const uint64_t s_Id = s_Bricks[i].m_RuntimeResourceID.GetID();
            p_List.emplace_back(s_Id, s_Bricks[i].m_EntityRef);
            if (p_Out.ids.size() < PlayerObservation::k_MaxBrickIds)
                p_Out.ids.push_back(PlayerObservation::Hex16(s_Id));
        }

        p_Out.truncated = Field<bool>::Observed(s_Count > PlayerObservation::k_MaxBrickIds);
        p_Out.scanned = Field<int64_t>::Observed(static_cast<int64_t>(s_Scanned));
    }

    // The owning entity named as a scanned brick's root, the scene root, or neither — "neither" is
    // established only when the scan was complete; otherwise the owner may lie beyond the bound.
    Field<std::string> BrickOf(const ZEntityRef& p_Owner, const ZEntitySceneContext* p_Context, const BrickList& p_Bricks, const PlayerObservation::BrickReads& p_Reads)
    {
        for (const auto& s_Brick : p_Bricks)
            if (s_Brick.second == p_Owner)
                return Field<std::string>::Observed(PlayerObservation::Hex16(s_Brick.first));

        if (p_Context && p_Context->m_pScene && p_Context->m_pScene.m_entityRef == p_Owner)
            return Field<std::string>::Observed("scene");

        return Field<std::string>::Of(FieldStatus::Unavailable, PlayerObservation::UnmatchedOwnerReason(p_Reads));
    }

    // The identity fields of one entity reference, all inline reads of declared SDK memory: the
    // entity type's authored id and the entity's flags, the owning entity's brick, the ZHitman5
    // fields when the interface resolved, and the spatial entity's stored translation with its
    // dirty flag (GetObjectToWorldMatrix is never called: it refreshes the engine's cache).
    void FillIdentity(const ZEntityRef& p_Ref, ZHitman5* p_Hero, const ZEntitySceneContext* p_Context, const BrickList& p_Bricks, const PlayerObservation::BrickReads& p_BrickReads, EntityIdentity& p_Out)
    {
        if (!p_Ref)
        {
            PlayerObservation::MarkIdentity(p_Out, FieldStatus::Unavailable, "entity reference null");
            return;
        }

        const ZEntityImpl* s_Entity = p_Ref.GetEntity();
        p_Out.resolved = Field<bool>::Observed(p_Hero != nullptr);

        const ZEntityType* s_Type = s_Entity->GetType();
        if (s_Type)
            p_Out.entity_id = Field<std::string>::Observed(PlayerObservation::Hex16(s_Type->m_nEntityID));
        else
            p_Out.entity_id = Field<std::string>::Of(FieldStatus::Unavailable, "entity type null");

        const int64_t s_Flags = static_cast<int64_t>(s_Entity->m_nEntityFlags);
        p_Out.entity_flags = Field<int64_t>::Observed(s_Flags);
        p_Out.activated = Field<bool>::Observed((s_Flags & PlayerObservation::k_FlagActivated) != 0);
        p_Out.predeleted = Field<bool>::Observed((s_Flags & PlayerObservation::k_FlagPredeleted) != 0);

        const ZEntityRef s_Owner = p_Ref.GetOwningEntity();
        p_Out.owning_brick = s_Owner ? BrickOf(s_Owner, p_Context, p_Bricks, p_BrickReads) : Field<std::string>::Of(FieldStatus::Unavailable, "no owning entity");

        if (p_Hero)
        {
            p_Out.character_id = Field<std::string>::Observed(RenderGuid(p_Hero->m_CharacterId));
            p_Out.initial_outfit_id = Field<std::string>::Observed(RenderGuid(p_Hero->m_InitialOutfitId));
            p_Out.invincible = Field<bool>::Observed(p_Hero->m_bIsInvincible);
        }
        else
        {
            p_Out.character_id = Field<std::string>::Of(FieldStatus::Unavailable, "hero interface not resolved");
            p_Out.initial_outfit_id = Field<std::string>::Of(FieldStatus::Unavailable, "hero interface not resolved");
            p_Out.invincible = Field<bool>::Of(FieldStatus::Unavailable, "hero interface not resolved");
        }

        const ZSpatialEntity* s_Spatial = p_Ref.QueryInterface<ZSpatialEntity>();
        if (!s_Spatial)
        {
            p_Out.x = Field<double>::Of(FieldStatus::Unavailable, "spatial interface not resolved");
            p_Out.y = Field<double>::Of(FieldStatus::Unavailable, "spatial interface not resolved");
            p_Out.z = Field<double>::Of(FieldStatus::Unavailable, "spatial interface not resolved");
            p_Out.transform_dirty = Field<bool>::Of(FieldStatus::Unavailable, "spatial interface not resolved");
            p_Out.room_behaviour = Field<int64_t>::Of(FieldStatus::Unavailable, "spatial interface not resolved");
            return;
        }

        p_Out.x = Field<double>::Observed(static_cast<double>(s_Spatial->m_mTransform.Trans.x));
        p_Out.y = Field<double>::Observed(static_cast<double>(s_Spatial->m_mTransform.Trans.y));
        p_Out.z = Field<double>::Observed(static_cast<double>(s_Spatial->m_mTransform.Trans.z));
        p_Out.transform_dirty = Field<bool>::Observed(s_Spatial->m_bWorldTransformDirty != 0);
        p_Out.room_behaviour = Field<int64_t>::Observed(static_cast<int64_t>(s_Spatial->m_eRoomBehaviour));
    }

    // The blueprint-factory lookup, each step its own boundary: the owner exists; its factory is
    // non-null; the factory's vtable is the template entity blueprint factory's; the sub-entity count,
    // valid (correction A: non-negative, within the plausibility bound) before the index of the
    // authored id is looked up; that index checked inside [0, count) before the one sub-entity access;
    // the sub-entity's equality with the reference entity (pointer equality on this frame — an
    // observation). A negative index is the factory's "not a sub-entity" answer; an index at or beyond
    // the count, or an invalid count, is an inconsistent factory answer, reported as such, and
    // GetSubEntity is never called for it. The resolved reference is handed back for this frame's
    // reads only.
    void Lookup(const ZEntityRef& p_Owner, uint64_t p_EntityId, const ZEntityRef& p_Reference, LookupFacts& p_Out, ZEntityRef* p_Resolved)
    {
        using PlayerObservation::IndexCheck;

        p_Out.owner_found = Field<bool>::Observed(static_cast<bool>(p_Owner));
        if (!p_Owner)
        {
            PlayerObservation::MarkLookup(p_Out, FieldStatus::Unavailable, "owner not found");
            p_Out.owner_found = Field<bool>::Observed(false);
            return;
        }

        ZEntityBlueprintFactoryBase* s_Factory = p_Owner.GetBlueprintFactory();
        p_Out.factory_available = Field<bool>::Observed(s_Factory != nullptr);
        if (!s_Factory)
        {
            p_Out.factory_template = Field<bool>::Of(FieldStatus::Unavailable, "blueprint factory null");
            p_Out.subentity_count = Field<int64_t>::Of(FieldStatus::Unavailable, "blueprint factory null");
            p_Out.index = Field<int64_t>::Of(FieldStatus::Unavailable, "blueprint factory null");
            p_Out.index_check = Field<std::string>::Of(FieldStatus::Unavailable, "blueprint factory null");
            p_Out.resolved = Field<bool>::Of(FieldStatus::Unavailable, "blueprint factory null");
            p_Out.same_entity = Field<bool>::Of(FieldStatus::Unavailable, "blueprint factory null");
            return;
        }

        const bool s_Template = s_Factory->IsTemplateEntityBlueprintFactory();
        p_Out.factory_template = Field<bool>::Observed(s_Template);
        if (!s_Template)
        {
            p_Out.subentity_count = Field<int64_t>::Of(FieldStatus::Unavailable, "not a template entity blueprint factory");
            p_Out.index = Field<int64_t>::Of(FieldStatus::Unavailable, "not a template entity blueprint factory");
            p_Out.index_check = Field<std::string>::Of(FieldStatus::Unavailable, "not a template entity blueprint factory");
            p_Out.resolved = Field<bool>::Of(FieldStatus::Unavailable, "not a template entity blueprint factory");
            p_Out.same_entity = Field<bool>::Of(FieldStatus::Unavailable, "not a template entity blueprint factory");
            return;
        }

        // The count as read, then its validity: an invalid count stops the lookup before the index
        // is even asked for (the factory's answers are not trusted past an invalid one).
        const int64_t s_Count = s_Factory->GetSubEntitiesCount();
        p_Out.subentity_count = Field<int64_t>::Observed(s_Count);
        if (!PlayerObservation::SubEntityCountValid(s_Count))
        {
            p_Out.index = Field<int64_t>::Of(FieldStatus::Unavailable, "count invalid; index not looked up");
            p_Out.index_check = Field<std::string>::Observed(PlayerObservation::IndexCheckText(IndexCheck::CountInvalid));
            p_Out.resolved = Field<bool>::Of(FieldStatus::Inconsistent, "sub-entity count invalid");
            p_Out.same_entity = Field<bool>::Of(FieldStatus::Inconsistent, "sub-entity count invalid");
            return;
        }

        const int64_t s_Index = s_Factory->GetSubEntityIndex(p_EntityId);
        p_Out.index = Field<int64_t>::Observed(s_Index);
        const IndexCheck s_Check = PlayerObservation::CheckSubEntityIndex(s_Count, s_Index);
        p_Out.index_check = Field<std::string>::Observed(PlayerObservation::IndexCheckText(s_Check));
        if (s_Check == IndexCheck::Negative)
        {
            p_Out.resolved = Field<bool>::Of(FieldStatus::Unavailable, "not a sub-entity of that factory");
            p_Out.same_entity = Field<bool>::Of(FieldStatus::Unavailable, "not a sub-entity of that factory");
            return;
        }
        if (s_Check != IndexCheck::InRange)
        {
            p_Out.resolved = Field<bool>::Of(FieldStatus::Inconsistent, "index outside the sub-entity count");
            p_Out.same_entity = Field<bool>::Of(FieldStatus::Inconsistent, "index outside the sub-entity count");
            return;
        }

        // The one sub-entity access, reached only with a valid count and an index inside it.
        const ZEntityRef s_Sub(s_Factory->GetSubEntity(p_Owner.m_pObj, static_cast<int>(s_Index)));
        p_Out.resolved = Field<bool>::Observed(static_cast<bool>(s_Sub));
        if (!s_Sub)
        {
            p_Out.same_entity = Field<bool>::Of(FieldStatus::Unavailable, "sub-entity null at a valid index");
            return;
        }

        if (p_Reference)
            p_Out.same_entity = Field<bool>::Observed(s_Sub == p_Reference);
        else
            p_Out.same_entity = Field<bool>::Of(FieldStatus::Unavailable, "local hero reference null");

        if (p_Resolved)
            *p_Resolved = s_Sub;
    }
}

PlayerObservation::RegistryReads SceneObservation::ObservePlayers()
{
    const ZPlayerRegistry* s_Registry = Globals::PlayerRegistry;

    if (!s_Registry)
    {
        RegistryReads s_None = PlayerObservation::AllRegistry(FieldStatus::Unavailable, "player registry global null");
        s_None.registry_available = Field<bool>::Observed(false);
        s_None.thread_id = GetCurrentThreadId();
        return s_None;
    }

    RegistryReads s_Reads;
    s_Reads.thread_id = GetCurrentThreadId();
    s_Reads.registry_available = Field<bool>::Observed(true);

    // The three words at 0x390 as values classified against the inline slots (experiment 4, F1).
    const auto* s_Words = reinterpret_cast<const uint64_t*>(&s_Registry->m_PlayerData);
    const uint64_t s_SlotsBase = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&s_Registry->m_aPlayerData[0]));
    std::string s_Classified;
    for (int i = 0; i < 3; ++i)
    {
        if (i > 0)
            s_Classified += ",";
        s_Classified += PlayerObservation::ClassifyWord(s_Words[i], s_SlotsBase, sizeof(SNetPlayerData), PlayerObservation::k_SlotCount);
    }
    s_Reads.words = Field<std::string>::Observed(s_Classified);

    ZHitman5* s_Slot0Hero = nullptr;

    for (size_t i = 0; i < PlayerObservation::k_SlotCount; ++i)
    {
        const SNetPlayerData& s_Data = s_Registry->m_aPlayerData[i];
        const ZNetPlayerController& s_Controller = s_Data.m_Controller;
        SlotReads& s_Out = s_Reads.slots[i];

        s_Out.player_id = Field<int64_t>::Observed(s_Data.m_nPlayerId);
        s_Out.local_flag = Field<bool>::Observed(s_Controller.m_bLocalPlayer);
        s_Out.connected_flag = Field<bool>::Observed(s_Controller.m_bConnectedToMultiplayer);
        s_Out.net_player = Field<bool>::Observed(s_Controller.m_pNetPlayer != nullptr);
        s_Out.replica = Field<bool>::Observed(s_Controller.m_pRakNetReplica != nullptr);

        const ZEntityRef s_Ref = s_Controller.m_HitmanEntity;
        s_Out.entity_present = Field<bool>::Observed(static_cast<bool>(s_Ref));

        if (s_Ref)
        {
            const TEntityRef<ZHitman5> s_Hero(s_Ref);
            s_Out.hero_resolved = Field<bool>::Observed(s_Hero.m_pInterfaceRef != nullptr);
            if (i == 0)
                s_Slot0Hero = s_Hero.m_pInterfaceRef;
        }
        else
        {
            s_Out.hero_resolved = Field<bool>::Of(FieldStatus::Unavailable, "entity reference null");
        }

        s_Out.flags_a0 = Field<int64_t>::Observed(static_cast<int64_t>(static_cast<int32_t>(s_Controller.m_nFlags0x98)));
        s_Out.session_id = ReadSessionField(s_Controller.s_sSessionId);
        s_Out.outfit_id = Field<std::string>::Observed(RenderGuid(s_Controller.m_OutfitId));
        s_Out.character_id = Field<std::string>::Observed(RenderGuid(s_Controller.m_SelectedCharacterId));
    }

    // Slot 0 through M5's local-hero path (the same entity, the same frame): its current kit id.
    if (!s_Slot0Hero)
        s_Reads.hero_kit_id = Field<std::string>::Of(FieldStatus::Unavailable, "hero interface not resolved");
    else if (!s_Slot0Hero->m_rOutfitKit || !s_Slot0Hero->m_rOutfitKit.m_pInterfaceRef)
        s_Reads.hero_kit_id = Field<std::string>::Of(FieldStatus::Unavailable, "outfit kit reference null");
    else
        s_Reads.hero_kit_id = Field<std::string>::Observed(RenderId(s_Slot0Hero->m_rOutfitKit.m_pInterfaceRef->m_sId));

    if (s_Reads.hero_kit_id.observed() && s_Reads.slots[0].outfit_id.observed())
        s_Reads.slot0_matches_m5 = Field<bool>::Observed(*s_Reads.hero_kit_id.value == *s_Reads.slots[0].outfit_id.value);
    else
        s_Reads.slot0_matches_m5 = Field<bool>::Of(FieldStatus::Unavailable, "kit id or registry outfit guid unavailable");

    return s_Reads;
}

PlayerObservation::EntityReads SceneObservation::ObserveEntity(const PlayerObservation::EntityQuery& p_Query)
{
    EntityReads s_Reads;
    s_Reads.thread_id = GetCurrentThreadId();

    const ZEntitySceneContext* s_Context = Globals::Hitman5Module ? Globals::Hitman5Module->m_pEntitySceneContext : nullptr;

    BrickList s_Bricks;
    ReadBricks(s_Context, s_Reads.bricks, s_Bricks);

    // The local hero: registry slot 0, exactly as M5's outfit path resolves it.
    ZEntityRef s_HeroRef;
    ZHitman5* s_Hero = nullptr;
    const ZPlayerRegistry* s_Registry = Globals::PlayerRegistry;

    if (!s_Registry)
    {
        PlayerObservation::MarkIdentity(s_Reads.hero, FieldStatus::Unavailable, "player registry global null");
        PlayerObservation::MarkLookup(s_Reads.round_trip, FieldStatus::Unavailable, "player registry global null");
    }
    else
    {
        s_HeroRef = s_Registry->m_aPlayerData[0].m_Controller.m_HitmanEntity;

        if (!s_HeroRef)
        {
            PlayerObservation::MarkIdentity(s_Reads.hero, FieldStatus::Unavailable, "hero entity reference null");
            PlayerObservation::MarkLookup(s_Reads.round_trip, FieldStatus::Unavailable, "hero entity reference null");
        }
        else
        {
            const TEntityRef<ZHitman5> s_HeroEntity(s_HeroRef);
            s_Hero = s_HeroEntity.m_pInterfaceRef;
            FillIdentity(s_HeroRef, s_Hero, s_Context, s_Bricks, s_Reads.bricks, s_Reads.hero);

            // The round trip: the hero's authored id looked up through its owning entity's factory.
            const ZEntityType* s_Type = s_HeroRef.GetEntity()->GetType();
            if (!s_Type)
                PlayerObservation::MarkLookup(s_Reads.round_trip, FieldStatus::Unavailable, "entity type null");
            else
                Lookup(s_HeroRef.GetOwningEntity(), s_Type->m_nEntityID, s_HeroRef, s_Reads.round_trip, nullptr);
        }
    }

    s_Reads.round_trip_outcome = PlayerObservation::ClassifyRoundTrip(s_Reads.round_trip);

    // The candidate: the named (or historical default) brick and authored entity id.
    PlayerObservation::CandidateReads s_Candidate;
    uint64_t s_BrickId = k_HitmenBrick.GetID();
    uint64_t s_EntityId = k_HitmenAgent48;
    bool s_IdsValid = true;
    if (p_Query.brick && !PlayerObservation::ParseHex16(*p_Query.brick, s_BrickId))
        s_IdsValid = false;
    if (p_Query.entity_id && !PlayerObservation::ParseHex16(*p_Query.entity_id, s_EntityId))
        s_IdsValid = false;
    s_Candidate.brick_id = PlayerObservation::Hex16(s_BrickId);
    s_Candidate.entity_id = PlayerObservation::Hex16(s_EntityId);

    const ZEntityRef* s_BrickRoot = nullptr;
    for (const auto& s_Brick : s_Bricks)
        if (s_Brick.first == s_BrickId)
        {
            s_BrickRoot = &s_Brick.second;
            break;
        }

    if (!s_IdsValid)
    {
        s_Candidate.brick_present = Field<bool>::Of(FieldStatus::Unavailable, "candidate id not parseable");
        PlayerObservation::MarkLookup(s_Candidate.lookup, FieldStatus::Unavailable, "candidate id not parseable");
        PlayerObservation::MarkIdentity(s_Candidate.identity, FieldStatus::Unavailable, "candidate id not parseable");
        s_Candidate.hitman5_resolved = Field<bool>::Of(FieldStatus::Unavailable, "candidate id not parseable");
    }
    else if (!s_Context)
    {
        s_Candidate.brick_present = Field<bool>::Of(FieldStatus::Unavailable, "scene context null");
        PlayerObservation::MarkLookup(s_Candidate.lookup, FieldStatus::Unavailable, "scene context null");
        PlayerObservation::MarkIdentity(s_Candidate.identity, FieldStatus::Unavailable, "scene context null");
        s_Candidate.hitman5_resolved = Field<bool>::Of(FieldStatus::Unavailable, "scene context null");
    }
    else if (!s_BrickRoot)
    {
        // Absent from the scanned prefix (correction C): the expected negative control without
        // content when the scan was complete; otherwise unavailable with the scan bound as the
        // reason, since the brick may lie beyond the bound.
        s_Candidate.brick_present = PlayerObservation::CandidatePresence(false, s_Reads.bricks);
        const std::string s_Why = s_Candidate.brick_present.observed() ? "candidate brick not loaded" : s_Candidate.brick_present.reason;
        PlayerObservation::MarkLookup(s_Candidate.lookup, FieldStatus::Unavailable, s_Why.c_str());
        PlayerObservation::MarkIdentity(s_Candidate.identity, FieldStatus::Unavailable, s_Why.c_str());
        s_Candidate.hitman5_resolved = Field<bool>::Of(FieldStatus::Unavailable, s_Why);
    }
    else
    {
        s_Candidate.brick_present = PlayerObservation::CandidatePresence(true, s_Reads.bricks);
        ZEntityRef s_Resolved;
        Lookup(*s_BrickRoot, s_EntityId, s_HeroRef, s_Candidate.lookup, &s_Resolved);

        if (s_Resolved)
        {
            const TEntityRef<ZHitman5> s_CandidateHero(s_Resolved);
            s_Candidate.hitman5_resolved = Field<bool>::Observed(s_CandidateHero.m_pInterfaceRef != nullptr);
            FillIdentity(s_Resolved, s_CandidateHero.m_pInterfaceRef, s_Context, s_Bricks, s_Reads.bricks, s_Candidate.identity);
        }
        else
        {
            PlayerObservation::MarkIdentity(s_Candidate.identity, FieldStatus::Unavailable, "candidate entity not resolved");
            s_Candidate.hitman5_resolved = Field<bool>::Of(FieldStatus::Unavailable, "candidate entity not resolved");
        }
    }

    s_Reads.candidate = s_Candidate;
    return s_Reads;
}
