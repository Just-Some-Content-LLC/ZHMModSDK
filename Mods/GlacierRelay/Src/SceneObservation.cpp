#include "SceneObservation.h"

#include "Globals.h"
#include "Glacier/ZApplicationEngineWin32.h"
#include "Glacier/ZModule.h"
#include "Glacier/ZPlayerRegistry.h"
#include "Glacier/ZScene.h"

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
