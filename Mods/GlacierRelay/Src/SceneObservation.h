#pragma once

#include <optional>
#include <string>

#include "Glacier/ZEntity.h"

#include "OutfitObservation.h"
#include "PlayerObservation.h"
#include "SceneState.h"

class ZHitman5;
class ZGlobalOutfitKit;

// Glacier-facing reads. These functions are the only place in the adapter that touches SDK types,
// apart from the plugin class itself and the outfit applier. Each is a read of engine state the
// Hitmen probe exercised in its first runtime load (glacier-relay HITMEN_COMPILE_ARCHAEOLOGY.md,
// experiment 4).
namespace SceneObservation
{
    // Scene resource, type, codename hint, loading stage and loaded flag from the scene context.
    SceneState ObserveScene();

    // The session id string of player slot 0, if the registry is present and the string plausible.
    // Observational payload only (experiment 4, F7): the adapter never keys anything on it.
    std::optional<std::string> ObserveGameSessionId();

    // M5 C-2B: the references the outfit applier needs on the applying frame — the hero and the
    // candidate kit as resolved by ObserveOutfit on that same frame. They are valid only for the
    // frame they were resolved on and are released by the applier before the frame ends.
    struct OutfitHandles
    {
        TEntityRef<ZHitman5> hero;
        TEntityRef<ZGlobalOutfitKit> kit;
    };

    // M5 C-1 (relay.inspect.outfit): the frame-local outfit inspection. Guarded reads only, each
    // reported with its own status: the registry session id read twice (a disagreement is
    // "inconsistent", never averaged), the local hero resolved fresh from registry slot 0 and its
    // current outfit kit's repository id, charset and variation with the HeroA bounds validated
    // before any indexed read, the optional candidate id resolved against the engine's own kit map
    // by a bounded byte comparison (never a pointer, never the Player mod's resource rewrite), and
    // whether the engine's outfit setter is resolved on this build. Nothing is written, nothing is
    // retained across frames, and no engine pointer leaves this function — except, for the outfit
    // applier only (M5 C-2B), into p_Handles when given.
    OutfitObservation::EngineReads ObserveOutfit(const std::optional<std::string>& p_CandidateId, OutfitHandles* p_Handles = nullptr);

    // M6-B1 (relay.inspect.players): the player registry as declared by the SDK, read on this frame
    // only — the three words at 0x390 classified against the inline slots (never interpreted as the
    // SDK's array model, which experiment 4 showed wrong on this build), the four inline slots' plain
    // fields, the ZHitman5 interface resolution of each slot's reference, and slot 0 through M5's
    // own local-hero path (its current kit id) beside the registry's outfit guid. No write, no
    // cache, no pointer leaves this function.
    PlayerObservation::RegistryReads ObservePlayers();

    // M6-B1 (relay.inspect.entity): the loaded-brick inventory (bounded), the local hero's authored
    // identity (entity id, flags, owning brick, character guid, initial outfit, invincibility) and
    // its stored transform with the dirty flag (never through the refreshing getter), the
    // blueprint-factory round trip of the hero through its owning entity, and the candidate brick /
    // authored entity (the historical hitmen.brick / Agent48 by default) with the same lookup. The
    // only engine calls are the SDK-declared virtual lookups on a blueprint factory
    // (GetSubEntitiesCount / GetSubEntityIndex / GetSubEntity), each reached only after a null and
    // a type check. No write, no cache, no pointer leaves this function.
    PlayerObservation::EntityReads ObserveEntity(const PlayerObservation::EntityQuery& p_Query);
}
