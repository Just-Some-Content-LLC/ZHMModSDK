#pragma once

#include <optional>
#include <string>

#include "SceneState.h"

// Glacier-facing reads. These functions are the only place in the adapter that touches SDK types,
// apart from the plugin class itself. Each is a read of engine state the Hitmen probe exercised in
// its first runtime load (glacier-relay HITMEN_COMPILE_ARCHAEOLOGY.md, experiment 4).
namespace SceneObservation
{
    // Scene resource, type, codename hint, loading stage and loaded flag from the scene context.
    SceneState ObserveScene();

    // The session id string of player slot 0, if the registry is present and the string plausible.
    // Observational payload only (experiment 4, F7): the adapter never keys anything on it.
    std::optional<std::string> ObserveGameSessionId();
}
