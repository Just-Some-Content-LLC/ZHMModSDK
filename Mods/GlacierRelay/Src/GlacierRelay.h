#pragma once

#include <memory>

#include "IPluginInterface.h"

#include "MissionObserver.h"
#include "RelayAdapter.h"
#include "SceneState.h"

struct SGameUpdateEvent;

// Glacier Relay native adapter: observe engine state each frame, derive the mission lifecycle
// edges (mission.playing, mission.stopped) and publish them through IRelaySink. No hooks, no UI,
// no inbound network, no writes to engine state.
class GlacierRelay : public IPluginInterface
{
public:
    GlacierRelay();
    ~GlacierRelay() override;

    void Init() override;
    void OnEngineInitialized() override;

private:
    void OnFrameUpdate(const SGameUpdateEvent& p_UpdateEvent);
    void ObserveFrame();

private:
    bool m_FrameUpdateRegistered = false;
    bool m_LoggedUnavailable = false;
    SceneState m_LastScene;

    MissionObserver m_MissionObserver;
    std::unique_ptr<RelayAdapter> m_Adapter;
};

DECLARE_ZHM_PLUGIN(GlacierRelay)
