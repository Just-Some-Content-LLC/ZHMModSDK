#include "OutfitApplier.h"

#include <Windows.h>

#include "Functions.h"
#include "Glacier/ZHitman5.h"
#include "Glacier/ZOutfit.h"

#include "RelayLog.h"

using OutfitObservation::Field;
using OutfitObservation::FieldStatus;

namespace
{
    OutfitObservation::EngineReads FaultedReads(const char* p_Why)
    {
        OutfitObservation::EngineReads s_Reads;
        s_Reads.thread_id = GetCurrentThreadId();
        s_Reads.generation_fresh = Field<std::string>::Of(FieldStatus::Unavailable, p_Why);
        s_Reads.hero.outfit_id = Field<std::string>::Of(FieldStatus::Unavailable, p_Why);
        s_Reads.hero.heroa_valid = Field<bool>::Of(FieldStatus::Unavailable, p_Why);
        s_Reads.set_outfit_available = Field<bool>::Of(FieldStatus::Unavailable, p_Why);
        s_Reads.kit_map_available = Field<bool>::Of(FieldStatus::Unavailable, p_Why);
        return s_Reads;
    }
}

OutfitObservation::EngineReads EngineOutfitApplier::Observe(const std::optional<std::string>& p_CandidateId)
{
    OutfitObservation::EngineReads s_Reads;
    bool s_Completed = false;

    RelayLog::Guard("OutfitApplier::Observe", [&] {
        s_Reads = SceneObservation::ObserveOutfit(p_CandidateId, &m_Handles);
        s_Completed = true;
    });

    if (!s_Completed)
    {
        m_Latched = true;
        m_Handles = {};
        return FaultedReads("fault during the apply-frame reads");
    }

    return s_Reads;
}

// The one call site. Everything it needs was resolved by Observe on this same frame; nothing is
// cached across frames; the setter's existence is checked again here.
OutfitApply::CallOutcome EngineOutfitApplier::Invoke(int32_t p_Charset, int32_t p_Variation)
{
    OutfitApply::CallOutcome s_Outcome;

    if (m_Latched || RelayLog::FaultsLogged() > 0)
        return s_Outcome;

    if (!m_Handles.hero || !m_Handles.hero.m_pInterfaceRef || !m_Handles.kit || !m_Handles.kit.m_pInterfaceRef)
        return s_Outcome;

    if (!Functions::ZHitman5_SetOutfit || !Functions::ZHitman5_SetOutfit->Exists())
        return s_Outcome;

    if (p_Charset < 0 || p_Variation < 0)
        return s_Outcome;

    ZHitman5* s_Hero = m_Handles.hero.m_pInterfaceRef;
    const TEntityRef<ZGlobalOutfitKit> s_Kit = m_Handles.kit;
    bool s_Returned = false;

    RelayLog::Guard("ApplyOutfit (ZHitman5_SetOutfit)", [&] {
        // HeroA only; the Player mod's flag values (bEnableOutfitModifiers false, bIgnoreOutifChange
        // false; the latter's semantics are not established by source). No shared kit data is
        // touched; the engine streams what it needs inside the call.
        Functions::ZHitman5_SetOutfit->Call(s_Hero, s_Kit, p_Charset, p_Variation, false, false);
        s_Returned = true;
    });

    s_Outcome.called = true;

    if (!s_Returned)
    {
        // Reachable only if an outer handler resumed the process after the fault (see the header).
        s_Outcome.faulted = true;
        m_Latched = true;
        RelayLog::Error("ApplyOutfit: the setter faulted and the process continued; the effectful path is latched off for the rest of this process (fail-stop); the effect is uncertain");
    }

    return s_Outcome;
}

OutfitObservation::EngineReads EngineOutfitApplier::ReadBack()
{
    OutfitObservation::EngineReads s_Reads;
    bool s_Completed = false;

    RelayLog::Guard("OutfitApplier::ReadBack", [&] {
        s_Reads = SceneObservation::ObserveOutfit(std::nullopt, nullptr);
        s_Completed = true;
    });

    if (!s_Completed)
    {
        m_Latched = true;
        return FaultedReads("fault during the read-back after the call");
    }

    return s_Reads;
}

bool EngineOutfitApplier::Latched() const
{
    return m_Latched || RelayLog::FaultsLogged() > 0;
}

void EngineOutfitApplier::Release()
{
    m_Handles = {};
}
