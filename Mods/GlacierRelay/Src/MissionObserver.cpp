#include "MissionObserver.h"

namespace
{
    constexpr int32_t k_LoadingStageScenePlaying = 8;
    constexpr std::string_view k_SceneTypeMission = "mission";

    MissionScenePayload PayloadFrom(const SceneState& p_Scene, const std::optional<std::string>& p_GameSessionId)
    {
        MissionScenePayload s_Payload;
        s_Payload.scene_resource = p_Scene.scene_resource;
        s_Payload.scene_type = p_Scene.scene_type;
        s_Payload.codename_hint = p_Scene.codename_hint;
        s_Payload.game_session_id = p_GameSessionId;
        return s_Payload;
    }
}

bool MissionObserver::IsMissionPlaying(const SceneState& p_Scene)
{
    return p_Scene.available
        && p_Scene.scene_type == k_SceneTypeMission
        && p_Scene.loading_stage == k_LoadingStageScenePlaying
        && p_Scene.scene_loaded;
}

std::optional<MissionEvent> MissionObserver::Update(
    const SceneState& p_Scene, const std::optional<std::string>& p_GameSessionId
)
{
    const bool s_WasPlaying = m_Playing;
    m_Playing = IsMissionPlaying(p_Scene);

    if (s_WasPlaying == m_Playing)
        return std::nullopt;

    if (m_Playing)
        return MissionPlayingEvent{PayloadFrom(p_Scene, p_GameSessionId)};

    // The payload is what the fall frame reports, not what the rise frame reported. In every
    // observed transition that is still the scene that was playing (the scene context is replaced
    // only by a later load, and never on a restart); if observability is lost instead, the fields
    // are empty and the event says so rather than repeating the rise.
    return MissionStoppedEvent{PayloadFrom(p_Scene, p_GameSessionId)};
}
