#include "MissionObserver.h"

namespace
{
    constexpr int32_t k_LoadingStageScenePlaying = 8;
    constexpr std::string_view k_SceneTypeMission = "mission";
}

bool MissionObserver::IsMissionPlaying(const SceneState& p_Scene)
{
    return p_Scene.available
        && p_Scene.scene_type == k_SceneTypeMission
        && p_Scene.loading_stage == k_LoadingStageScenePlaying
        && p_Scene.scene_loaded;
}

std::optional<MissionPlayingEvent> MissionObserver::Update(
    const SceneState& p_Scene, const std::optional<std::string>& p_GameSessionId
)
{
    const bool s_WasPlaying = m_Playing;
    m_Playing = IsMissionPlaying(p_Scene);

    if (s_WasPlaying || !m_Playing)
        return std::nullopt;

    MissionPlayingEvent s_Event;
    s_Event.scene_resource = p_Scene.scene_resource;
    s_Event.scene_type = p_Scene.scene_type;
    s_Event.codename_hint = p_Scene.codename_hint;
    s_Event.game_session_id = p_GameSessionId;

    return s_Event;
}
