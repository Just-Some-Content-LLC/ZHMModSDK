#include "SceneObservation.h"

#include <Windows.h>

#include <array>
#include <cstring>

#include "Functions.h"
#include "Globals.h"
#include "Glacier/Enums.h"
#include "Glacier/ZApplicationEngineWin32.h"
#include "Glacier/ZContentKitManager.h"
#include "Glacier/ZHitman5.h"
#include "Glacier/ZModule.h"
#include "Glacier/ZOutfit.h"
#include "Glacier/ZPlayerRegistry.h"
#include "Glacier/ZScene.h"

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

OutfitObservation::EngineReads SceneObservation::ObserveOutfit(const std::optional<std::string>& p_CandidateId)
{
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
                    s_Resolution.found = Field<bool>::Of(FieldStatus::Unavailable, "kit entry reference null");
                else
                    DescribeKit(*s_Pair.second.m_pInterfaceRef, s_Resolution);
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
