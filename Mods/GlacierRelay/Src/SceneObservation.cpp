#include "SceneObservation.h"

#include <Windows.h>

#include <array>
#include <cstring>

#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

#include "Functions.h"
#include "Globals.h"
#include "Glacier/CompileReflection.h"
#include "Glacier/EntityFactory.h"
#include "Glacier/Hash.h"
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

// ---- M6-D1a: the read-only candidate-hero inspection -----------------------------------------------
namespace
{
    using CandidateObservation::CandidateReads;
    using CandidateObservation::HeroControllers;
    using CandidateObservation::HeroMembers;
    using CandidateObservation::HeroReads;
    using CandidateObservation::LocalTransform;
    using CandidateObservation::Member;
    using CandidateObservation::OutfitTuple;
    using CandidateObservation::SlotIdentity;
    using CandidateObservation::SlotMatch;
    using CandidateObservation::SpatialFlags;
    using CandidateObservation::SpawnReads;
    using CandidateObservation::SpatialReads;
    using CandidateObservation::Vec3;

    // The historical spawn (Agent48Spawn): content, not a pointer; never a peer-supplied id.
    constexpr uint64_t k_HitmenAgent48Spawn = CandidateObservation::k_DefaultSpawnId;

    // The property ids the inspection may read, fixed at compile time (the SDK's own hashing of the
    // property name); no name reaches this code from the wire.
    constexpr uint32_t k_PropActive = Hash::Crc32("m_bActive");
    constexpr uint32_t k_PropPlayerId = Hash::Crc32("m_nPlayerId");
    constexpr uint32_t k_PropDefaultSpawn = Hash::Crc32("m_bDefaultHeroSpawn");
    constexpr uint32_t k_PropPreferredSpawn = Hash::Crc32("m_bPreferredHeroSpawn");
    constexpr uint32_t k_PropHitmanCharacter = Hash::Crc32("m_rHitmanCharacter");
    constexpr size_t k_MaxTypeNameBytes = 256; // the bound of the type-name scan in the engine's type table

    static_assert(sizeof(ZSpatialEntity) == 0xA0, "ZSpatialEntity is expected to be 0xA0 bytes");
    static_assert(offsetof(ZSpatialEntity, m_eidParent) == 0x70, "m_eidParent is read at 0x70");
    static_assert(offsetof(ZSpatialEntity, m_pTransformParent) == 0x80, "m_pTransformParent is read at 0x80");
    static_assert(offsetof(ZHitman5, m_pCharacter) == 0xF00, "ZHitman5::m_pCharacter is read at 0xF00");
    static_assert(offsetof(ZHitman5, m_rMainCamera) == 0x1348, "ZHitman5::m_rMainCamera is read at 0x1348");

    // Member states: a pointer or entity reference is present when non-null; an entity reference is
    // valid when its interface also resolved. Nothing is dereferenced to decide either.
    Member PointerState(const void* p_Pointer)
    {
        return p_Pointer ? Member::Present : Member::Null;
    }

    Member RefState(const ZEntityRef& p_Ref)
    {
        return p_Ref ? Member::Present : Member::Null;
    }

    template <class T>
    Member TypedRefState(const TEntityRef<T>& p_Ref)
    {
        if (!p_Ref.m_entityRef)
            return Member::Null;
        return p_Ref.m_pInterfaceRef ? Member::Valid : Member::Present;
    }

    // The authored entity id of a reference's type ("none" for a null reference), through the same
    // declared accessors FillIdentity uses.
    Field<std::string> AuthoredIdOf(const ZEntityRef& p_Ref)
    {
        if (!p_Ref)
            return Field<std::string>::Observed("none");

        const ZEntityType* s_Type = p_Ref.GetEntity()->GetType();
        if (!s_Type)
            return Field<std::string>::Of(FieldStatus::Unavailable, "entity type null");

        return Field<std::string>::Observed(PlayerObservation::Hex16(s_Type->m_nEntityID));
    }

    // The STORED spatial fields of an entity, read as declared SDK members: the cached
    // object-to-world translation (GetObjectToWorldMatrix is never called: it refreshes the cache),
    // the parent-relative translation and rotation, the bitfield, the parent reference's authored
    // id, and the presence of the transform-tree pointers (compared with null, never dereferenced).
    void ReadSpatial(const ZEntityRef& p_Ref, SpatialReads& p_Out)
    {
        const ZSpatialEntity* s_Spatial = p_Ref.QueryInterface<ZSpatialEntity>();
        if (!s_Spatial)
        {
            CandidateObservation::MarkSpatial(p_Out, FieldStatus::Unavailable, "spatial interface not resolved");
            return;
        }

        Vec3 s_World;
        s_World.x = s_Spatial->m_mTransform.Trans.x;
        s_World.y = s_Spatial->m_mTransform.Trans.y;
        s_World.z = s_Spatial->m_mTransform.Trans.z;
        p_Out.world = Field<Vec3>::Observed(s_World);

        LocalTransform s_Local;
        s_Local.translation.x = s_Spatial->m_vObjectToParentTranslation.x;
        s_Local.translation.y = s_Spatial->m_vObjectToParentTranslation.y;
        s_Local.translation.z = s_Spatial->m_vObjectToParentTranslation.z;
        s_Local.rotation.x = s_Spatial->m_vObjectToParentRotation.x;
        s_Local.rotation.y = s_Spatial->m_vObjectToParentRotation.y;
        s_Local.rotation.z = s_Spatial->m_vObjectToParentRotation.z;
        s_Local.rotation.w = s_Spatial->m_vObjectToParentRotation.w;
        p_Out.local = Field<LocalTransform>::Observed(s_Local);

        SpatialFlags s_Flags;
        s_Flags.visible = s_Spatial->m_bVisible != 0;
        s_Flags.parent_hidden = s_Spatial->m_bParentHidden != 0;
        s_Flags.force_visible = s_Spatial->m_bForceVisible != 0;
        s_Flags.dynamic = s_Spatial->m_bIsDynamic != 0;
        s_Flags.scene_child = s_Spatial->m_bSceneChild != 0;
        s_Flags.dirty = s_Spatial->m_bWorldTransformDirty != 0;
        s_Flags.room_behaviour = static_cast<int64_t>(s_Spatial->m_eRoomBehaviour);
        s_Flags.transform_parent = s_Spatial->m_pTransformParent != nullptr;
        s_Flags.transform_children = s_Spatial->m_pTransformChildren != nullptr;
        p_Out.flags = Field<SpatialFlags>::Observed(s_Flags);

        p_Out.parent = AuthoredIdOf(s_Spatial->m_eidParent.m_entityRef);
    }

    // One property through the SDK's documented mechanism (ZEntityRef::GetProperty), reached only
    // after every step is verified: the entity and its type table exist, the property is declared
    // on that type, it carries no engine getter (a getter is an engine call with unknown side
    // effects; such a property is reported unsupported, not read), its registered type is exactly
    // T by name and size, and T is registered. GetProperty copies the value through the type's own
    // copy function into a variant allocated by the engine's allocator; the variant is destroyed —
    // and its memory released — before this function returns. Nothing is written to the entity.
    template <class T>
    Field<T> ReadProperty(const ZEntityRef& p_Ref, uint32_t p_PropertyId)
    {
        if (!p_Ref)
            return Field<T>::Of(FieldStatus::Unavailable, "entity reference null");

        const ZEntityType* s_Type = p_Ref.GetEntity()->GetType();
        if (!s_Type || !s_Type->m_pPropertyData)
            return Field<T>::Of(FieldStatus::Unavailable, "entity type or property table null");

        if (!Globals::TypeRegistry || !*Globals::TypeRegistry || !Globals::MemoryManager || !*Globals::MemoryManager)
            return Field<T>::Of(FieldStatus::Unavailable, "type registry or memory manager null");

        const SPropertyData* s_Property = s_Type->FindProperty(p_PropertyId);
        if (!s_Property)
            return Field<T>::Of(FieldStatus::Unavailable, "property not declared on this type");

        const SNamedPropertyInfo* s_Info = s_Property->GetPropertyInfo();
        if (!s_Info || !s_Info->m_propertyInfo.m_Type)
            return Field<T>::Of(FieldStatus::Unavailable, "property info null");

        if (s_Info->m_propertyInfo.m_Flags & EPropertyInfoFlags::E_HAS_GETTER_SETTER)
            return Field<T>::Of(FieldStatus::Unsupported, "property has an engine getter");

        const IType* s_TypeInfo = s_Info->m_propertyInfo.m_Type->GetTypeInfo();
        if (!s_TypeInfo || !s_TypeInfo->pszTypeName)
            return Field<T>::Of(FieldStatus::Unavailable, "property type info null");

        const size_t s_NameLength = strnlen(s_TypeInfo->pszTypeName, k_MaxTypeNameBytes);
        if (Hash::Crc32(s_TypeInfo->pszTypeName, s_NameLength) != ZHMTypeId<T> || s_TypeInfo->m_nTypeSize != sizeof(T))
            return Field<T>::Of(FieldStatus::Unsupported, "property type " + std::string(s_TypeInfo->pszTypeName, s_NameLength));

        if (!(*Globals::TypeRegistry)->GetTypeID(ZHMTypeName<T>))
            return Field<T>::Of(FieldStatus::Unsupported, "type not registered");

        {
            const ZVariant<T> s_Variant = p_Ref.GetProperty<T>(p_PropertyId);
            const T* s_Value = s_Variant.As<T>();
            if (!s_Value)
                return Field<T>::Of(FieldStatus::Unavailable, "property variant type mismatch");
            return Field<T>::Observed(*s_Value);
        } // the variant releases the engine allocation here
    }

    // The class of an entity as its own interface table names it: "ZHeroSpawn" when that interface
    // is listed, otherwise the first named interface (bounded). A read of the type table only.
    Field<std::string> ClassNameOf(const ZEntityRef& p_Ref)
    {
        const ZEntityType* s_Type = p_Ref.GetEntity()->GetType();
        if (!s_Type || !s_Type->m_pInterfaceData)
            return Field<std::string>::Of(FieldStatus::Unavailable, "entity type or interface table null");

        std::string s_First;
        bool s_HeroSpawn = false;
        size_t s_Scanned = 0;

        for (const auto& s_Interface : *s_Type->m_pInterfaceData)
        {
            if (++s_Scanned > CandidateObservation::k_MaxInterfacesScanned)
                break;
            if (!s_Interface.m_Type)
                continue;
            const IType* s_TypeInfo = s_Interface.m_Type->GetTypeInfo();
            if (!s_TypeInfo || !s_TypeInfo->pszTypeName)
                continue;
            const std::string_view s_Name(s_TypeInfo->pszTypeName, strnlen(s_TypeInfo->pszTypeName, k_MaxTypeNameBytes));
            if (s_Name == "ZHeroSpawn")
                s_HeroSpawn = true;
            if (s_First.empty())
                s_First = std::string(s_Name.substr(0, CandidateObservation::k_MaxClassNameBytes));
        }

        if (s_HeroSpawn)
            return Field<std::string>::Observed("ZHeroSpawn");
        if (s_First.empty())
            return Field<std::string>::Of(FieldStatus::Unavailable, "no named interface");
        return Field<std::string>::Observed(s_First);
    }

    // One hero's state beyond its identity: the stored spatial fields, the member and controller
    // presence, the current outfit tuple and the m_bActive property. Presence is never simulation.
    void ReadHero(const ZEntityRef& p_Ref, const ZHitman5* p_Hero, HeroReads& p_Out)
    {
        ReadSpatial(p_Ref, p_Out.spatial);

        if (!p_Hero)
        {
            p_Out.members = Field<HeroMembers>::Of(FieldStatus::Unavailable, "hero interface not resolved");
            p_Out.controllers = Field<HeroControllers>::Of(FieldStatus::Unavailable, "hero interface not resolved");
            p_Out.outfit = Field<OutfitTuple>::Of(FieldStatus::Unavailable, "hero interface not resolved");
            p_Out.active = Field<bool>::Of(FieldStatus::Unavailable, "hero interface not resolved");
            return;
        }

        HeroMembers s_Members;
        s_Members.character = TypedRefState(p_Hero->m_pCharacter);
        s_Members.animator = RefState(p_Hero->m_Animator);
        s_Members.morpheme = RefState(p_Hero->m_MorphemeEntityID);
        s_Members.ragdoll = TypedRefState(p_Hero->m_rRagdollEntity);
        s_Members.health = PointerState(p_Hero->m_pHealth);
        s_Members.outfit_kit = TypedRefState(p_Hero->m_rOutfitKit);
        s_Members.main_camera = TypedRefState(p_Hero->m_rMainCamera);
        s_Members.input_processor = PointerState(p_Hero->m_pCharacterInputProcessor);
        p_Out.members = Field<HeroMembers>::Observed(s_Members);

        HeroControllers s_Controllers;
        s_Controllers.guide = TypedRefState(p_Hero->m_pGuideController);
        s_Controllers.ik = TypedRefState(p_Hero->m_pIKController);
        s_Controllers.cheat = TypedRefState(p_Hero->m_pCheatController);
        s_Controllers.interaction = TypedRefState(p_Hero->m_pInteractionController);
        s_Controllers.actor_tag = TypedRefState(p_Hero->m_pActorTagController);
        p_Out.controllers = Field<HeroControllers>::Observed(s_Controllers);

        OutfitTuple s_Outfit;
        s_Outfit.kit_id = p_Hero->m_rOutfitKit && p_Hero->m_rOutfitKit.m_pInterfaceRef ? RenderId(p_Hero->m_rOutfitKit.m_pInterfaceRef->m_sId) : std::string("none");
        s_Outfit.charset = p_Hero->m_nOutfitCharset;
        s_Outfit.variation = p_Hero->m_nOutfitVariation;
        p_Out.outfit = Field<OutfitTuple>::Observed(s_Outfit);

        p_Out.active = ReadProperty<bool>(p_Ref, k_PropActive);
    }

    // The spawn entity: its class, its stored transform and dirty flag, and its four authored
    // properties through the property mechanism; the hero reference resolved to an authored id and
    // compared by identity with the candidate on this frame (an authored relationship, never
    // evidence that the spawn system ran).
    void ReadSpawn(const ZEntityRef& p_Ref, const ZEntityRef& p_Candidate, SpawnReads& p_Out)
    {
        p_Out.class_name = ClassNameOf(p_Ref);

        const ZSpatialEntity* s_Spatial = p_Ref.QueryInterface<ZSpatialEntity>();
        if (!s_Spatial)
        {
            p_Out.world = Field<Vec3>::Of(FieldStatus::Unavailable, "spatial interface not resolved");
            p_Out.dirty = Field<bool>::Of(FieldStatus::Unavailable, "spatial interface not resolved");
        }
        else
        {
            Vec3 s_World;
            s_World.x = s_Spatial->m_mTransform.Trans.x;
            s_World.y = s_Spatial->m_mTransform.Trans.y;
            s_World.z = s_Spatial->m_mTransform.Trans.z;
            p_Out.world = Field<Vec3>::Observed(s_World);
            p_Out.dirty = Field<bool>::Observed(s_Spatial->m_bWorldTransformDirty != 0);
        }

        const Field<int32_t> s_PlayerId = ReadProperty<int32_t>(p_Ref, k_PropPlayerId);
        p_Out.player_id = s_PlayerId.observed() ? Field<int64_t>::Observed(*s_PlayerId.value) : Field<int64_t>::Of(s_PlayerId.status, s_PlayerId.reason);
        p_Out.default_spawn = ReadProperty<bool>(p_Ref, k_PropDefaultSpawn);
        p_Out.preferred_spawn = ReadProperty<bool>(p_Ref, k_PropPreferredSpawn);

        const Field<TEntityRef<ZHitman5>> s_Ref = ReadProperty<TEntityRef<ZHitman5>>(p_Ref, k_PropHitmanCharacter);
        if (!s_Ref.observed())
        {
            p_Out.hitman_ref = Field<std::string>::Of(s_Ref.status, s_Ref.reason);
            p_Out.ref_is_candidate = Field<bool>::Of(s_Ref.status, s_Ref.reason);
            return;
        }

        const ZEntityRef s_Target = s_Ref.value->m_entityRef; // this frame only; not kept
        p_Out.hitman_ref = AuthoredIdOf(s_Target);
        if (!s_Target)
            p_Out.ref_is_candidate = Field<bool>::Observed(false);
        else if (p_Candidate)
            p_Out.ref_is_candidate = Field<bool>::Observed(s_Target == p_Candidate);
        else
            p_Out.ref_is_candidate = Field<bool>::Of(FieldStatus::Unavailable, "candidate entity not resolved");
    }

    using CandidateObservation::CandidateKnowledge;

    // The four slots' m_HitmanEntity compared by entity identity on this frame with the local hero
    // and the candidate (pointer equality through ZEntityRef, never kept), classified by the pure
    // rule; the ZHitman5 resolution of each slot's reference recorded beside it.
    Field<std::string> ReadSlots(const ZPlayerRegistry* p_Registry, const ZEntityRef& p_Hero, const ZEntityRef& p_Candidate, CandidateKnowledge p_Knowledge)
    {
        if (!p_Registry)
            return Field<std::string>::Of(FieldStatus::Unavailable, "player registry global null");

        SlotIdentity s_Slots[CandidateObservation::k_SlotCount];
        for (size_t i = 0; i < CandidateObservation::k_SlotCount; ++i)
        {
            const ZEntityRef s_Ref = p_Registry->m_aPlayerData[i].m_Controller.m_HitmanEntity;
            const bool s_Present = static_cast<bool>(s_Ref);
            s_Slots[i].hero_resolved = s_Present && TEntityRef<ZHitman5>(s_Ref).m_pInterfaceRef != nullptr;
            s_Slots[i].match = CandidateObservation::ClassifySlot(
                s_Present,
                s_Present && p_Hero && s_Ref == p_Hero,
                s_Present && p_Candidate && s_Ref == p_Candidate,
                p_Knowledge
            );
        }

        return Field<std::string>::Observed(CandidateObservation::EncodeSlots(s_Slots));
    }

    void MarkCandidateBlock(CandidateReads& p_Reads, FieldStatus p_Status, const char* p_Reason, bool p_IncludeLookup)
    {
        if (p_IncludeLookup)
            PlayerObservation::MarkLookup(p_Reads.lookup, p_Status, p_Reason);
        p_Reads.hitman5 = Field<bool>::Of(p_Status, p_Reason);
        p_Reads.cand_flags = Field<int64_t>::Of(p_Status, p_Reason);
        p_Reads.id_match = Field<bool>::Of(p_Status, p_Reason);
        p_Reads.cand_brick_owner = Field<std::string>::Of(p_Status, p_Reason);
        CandidateObservation::MarkHero(p_Reads.candidate, p_Status, p_Reason);
    }
}

CandidateObservation::CandidateReads SceneObservation::ObserveCandidate(const CandidateObservation::CandidateQuery& p_Query)
{
    CandidateReads s_Reads;
    s_Reads.thread_id = GetCurrentThreadId();

    const ZEntitySceneContext* s_Context = Globals::Hitman5Module ? Globals::Hitman5Module->m_pEntitySceneContext : nullptr;
    const ZPlayerRegistry* s_Registry = Globals::PlayerRegistry;

    // 1. The mission generation on this frame: registry slot 0's session id read twice.
    if (!s_Registry)
    {
        s_Reads.generation_fresh = Field<std::string>::Of(FieldStatus::Unavailable, "player registry global null");
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
    }

    // 2. The loaded bricks (bounded, as M6-B1).
    BrickList s_Bricks;
    ReadBricks(s_Context, s_Reads.bricks, s_Bricks);

    // 3. The local hero: registry slot 0, exactly as M5's outfit path resolves it; the control.
    ZEntityRef s_HeroRef;
    ZHitman5* s_Hero = nullptr;
    if (!s_Registry)
    {
        s_Reads.hero_entity_id = Field<std::string>::Of(FieldStatus::Unavailable, "player registry global null");
        s_Reads.hero_flags = Field<int64_t>::Of(FieldStatus::Unavailable, "player registry global null");
        CandidateObservation::MarkHero(s_Reads.hero, FieldStatus::Unavailable, "player registry global null");
    }
    else
    {
        s_HeroRef = s_Registry->m_aPlayerData[0].m_Controller.m_HitmanEntity;
        if (!s_HeroRef)
        {
            s_Reads.hero_entity_id = Field<std::string>::Of(FieldStatus::Unavailable, "hero entity reference null");
            s_Reads.hero_flags = Field<int64_t>::Of(FieldStatus::Unavailable, "hero entity reference null");
            CandidateObservation::MarkHero(s_Reads.hero, FieldStatus::Unavailable, "hero entity reference null");
        }
        else
        {
            const TEntityRef<ZHitman5> s_HeroEntity(s_HeroRef);
            s_Hero = s_HeroEntity.m_pInterfaceRef;
            s_Reads.hero_entity_id = AuthoredIdOf(s_HeroRef);
            s_Reads.hero_flags = Field<int64_t>::Observed(static_cast<int64_t>(s_HeroRef.GetEntity()->m_nEntityFlags));
            ReadHero(s_HeroRef, s_Hero, s_Reads.hero);
        }
    }

    // 4. The candidate and its spawn: the named (or historical) brick and authored entity id; the
    //    spawn id is the compiled historical default.
    uint64_t s_BrickId = k_HitmenBrick.GetID();
    uint64_t s_EntityId = k_HitmenAgent48;
    bool s_IdsValid = true;
    if (p_Query.brick && !PlayerObservation::ParseHex16(*p_Query.brick, s_BrickId))
        s_IdsValid = false;
    if (p_Query.entity_id && !PlayerObservation::ParseHex16(*p_Query.entity_id, s_EntityId))
        s_IdsValid = false;
    s_Reads.brick_id = PlayerObservation::Hex16(s_BrickId);
    s_Reads.entity_id = PlayerObservation::Hex16(s_EntityId);
    s_Reads.spawn_id = PlayerObservation::Hex16(k_HitmenAgent48Spawn);

    const ZEntityRef* s_BrickRoot = nullptr;
    for (const auto& s_Brick : s_Bricks)
        if (s_Brick.first == s_BrickId)
        {
            s_BrickRoot = &s_Brick.second;
            break;
        }

    ZEntityRef s_Resolved;

    if (!s_IdsValid)
    {
        s_Reads.brick_present = Field<bool>::Of(FieldStatus::Unavailable, "candidate id not parseable");
        MarkCandidateBlock(s_Reads, FieldStatus::Unavailable, "candidate id not parseable", true);
        CandidateObservation::MarkSpawn(s_Reads.spawn, FieldStatus::Unavailable, "candidate id not parseable");
    }
    else if (!s_Context)
    {
        s_Reads.brick_present = Field<bool>::Of(FieldStatus::Unavailable, "scene context null");
        MarkCandidateBlock(s_Reads, FieldStatus::Unavailable, "scene context null", true);
        CandidateObservation::MarkSpawn(s_Reads.spawn, FieldStatus::Unavailable, "scene context null");
    }
    else if (!s_BrickRoot)
    {
        // Absent from the scanned prefix: "false" only from a complete scan (correction C); the
        // presence field carries the scan's own reason, the cascade a short one (the line budget).
        s_Reads.brick_present = PlayerObservation::CandidatePresence(false, s_Reads.bricks);
        const char* s_Why = s_Reads.brick_present.observed() ? "brick not loaded" : "brick scan incomplete";
        MarkCandidateBlock(s_Reads, FieldStatus::Unavailable, s_Why, true);
        CandidateObservation::MarkSpawn(s_Reads.spawn, FieldStatus::Unavailable, s_Why);
    }
    else
    {
        s_Reads.brick_present = PlayerObservation::CandidatePresence(true, s_Reads.bricks);
        Lookup(*s_BrickRoot, s_EntityId, s_HeroRef, s_Reads.lookup, &s_Resolved);

        if (s_Resolved)
        {
            const TEntityRef<ZHitman5> s_CandidateHero(s_Resolved);
            s_Reads.hitman5 = Field<bool>::Observed(s_CandidateHero.m_pInterfaceRef != nullptr);
            s_Reads.cand_flags = Field<int64_t>::Observed(static_cast<int64_t>(s_Resolved.GetEntity()->m_nEntityFlags));

            const ZEntityType* s_Type = s_Resolved.GetEntity()->GetType();
            s_Reads.id_match = s_Type ? Field<bool>::Observed(s_Type->m_nEntityID == s_EntityId) : Field<bool>::Of(FieldStatus::Unavailable, "entity type null");

            const ZEntityRef s_Owner = s_Resolved.GetOwningEntity();
            s_Reads.cand_brick_owner = s_Owner ? BrickOf(s_Owner, s_Context, s_Bricks, s_Reads.bricks) : Field<std::string>::Of(FieldStatus::Unavailable, "no owning entity");

            ReadHero(s_Resolved, s_CandidateHero.m_pInterfaceRef, s_Reads.candidate);
        }
        else
        {
            MarkCandidateBlock(s_Reads, FieldStatus::Unavailable, "candidate entity not resolved", false);
        }

        // The spawn: the same factory, the same bounds, the reference entity being the candidate.
        ZEntityRef s_SpawnRef;
        Lookup(*s_BrickRoot, k_HitmenAgent48Spawn, s_Resolved, s_Reads.spawn.lookup, &s_SpawnRef);
        if (s_SpawnRef)
        {
            ReadSpawn(s_SpawnRef, s_Resolved, s_Reads.spawn);
        }
        else
        {
            const PlayerObservation::LookupFacts s_Facts = s_Reads.spawn.lookup;
            CandidateObservation::MarkSpawn(s_Reads.spawn, FieldStatus::Unavailable, "spawn entity not resolved");
            s_Reads.spawn.lookup = s_Facts;
        }
    }

    // 5. Registry membership by identity on this frame.
    s_Reads.registry_slots = ReadSlots(s_Registry, s_HeroRef, s_Resolved, CandidateObservation::KnowledgeOf(static_cast<bool>(s_Resolved), s_Reads.brick_present));

    return s_Reads;
}
