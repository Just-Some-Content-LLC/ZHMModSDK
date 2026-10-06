#pragma once

#include "IPluginInterface.h"

// Glacier Relay native adapter, M1 stage 1: observe engine state, derive one semantic event,
// publish it to a log-only sink. No hooks, no UI, no network, no writes to engine state.
class GlacierRelay : public IPluginInterface
{
public:
    GlacierRelay();
    ~GlacierRelay() override;

    void Init() override;
    void OnEngineInitialized() override;
};

DECLARE_ZHM_PLUGIN(GlacierRelay)
