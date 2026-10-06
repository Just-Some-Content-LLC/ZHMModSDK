#pragma once

#include <cstdint>
#include <string>

// What the adapter knows about the current scene, as plain owned values. This is the output of the
// Glacier-facing observation layer and the only scene input the semantic layer sees. It must never
// grow a pointer, an SDK type or an offset.
struct SceneState
{
    // False when the engine globals needed to read the rest were not available; the other fields
    // are then meaningless.
    bool available = false;

    std::string scene_resource; // e.g. assembly:/_PRO/Scenes/Missions/Paris/_Scene_FashionShowHit_01.entity
    std::string scene_type;     // "mission" for missions; empty for the frontend (observed 2026-10-06)
    std::string codename_hint;  // e.g. "Peacock"
    int32_t loading_stage = -1; // ESceneLoadingStage as an integer; 8 is eLoading_ScenePlaying
    bool scene_loaded = false;  // the engine's scene-loaded flag together with a non-null scene

    bool operator==(const SceneState&) const = default;
};
