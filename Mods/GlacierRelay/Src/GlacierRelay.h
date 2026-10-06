#pragma once

#include <memory>

#include "IPluginInterface.h"

#include "MissionObserver.h"
#include "RelayAdapter.h"
#include "SceneState.h"

struct SGameUpdateEvent;

// Glacier Relay native adapter, M1 stage 1: observe engine state, derive one semantic event,
// publish it to a log-only sink. No hooks, no UI, no network, no writes to engine state.
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
