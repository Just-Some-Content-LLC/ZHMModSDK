#pragma once

#include <optional>
#include <string>

#include "OutfitObservation.h"
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

    // M5 C-1 (relay.inspect.outfit): the frame-local outfit inspection. Guarded reads only, each
    // reported with its own status: the registry session id read twice (a disagreement is
    // "inconsistent", never averaged), the local hero resolved fresh from registry slot 0 and its
    // current outfit kit's repository id, charset and variation with the HeroA bounds validated
    // before any indexed read, the optional candidate id resolved against the engine's own kit map
    // by a bounded byte comparison (never a pointer, never the Player mod's resource rewrite), and
    // whether the engine's outfit setter is resolved on this build. Nothing is written, nothing is
    // retained across frames, and no engine pointer leaves this function.
    OutfitObservation::EngineReads ObserveOutfit(const std::optional<std::string>& p_CandidateId);
}
