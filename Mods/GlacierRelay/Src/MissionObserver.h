#pragma once

#include <optional>
#include <string>

#include "RelayEvent.h"
#include "SceneState.h"

// The semantic layer for M1: one predicate over observed scene state, and edge detection on it.
// Pure: no engine access, no clock, no I/O. Fed once per frame by the plugin.
class MissionObserver
{
public:
    // The state predicate. True while the engine reports a mission scene as playing:
    // scene type "mission", loading stage 8 (eLoading_ScenePlaying) and the scene-loaded flag.
    // Unobservable state counts as not playing.
    static bool IsMissionPlaying(const SceneState& p_Scene);

    // Feeds one observation. Returns an event only when the predicate goes from false to true.
    // The session id is attached to the event as observational payload and does not affect the
    // predicate or the edge.
    std::optional<MissionPlayingEvent> Update(
        const SceneState& p_Scene, const std::optional<std::string>& p_GameSessionId
    );

    bool Playing() const { return m_Playing; }

private:
    bool m_Playing = false;
};
