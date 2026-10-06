#pragma once

#include <optional>
#include <string>
#include <variant>

// Semantic events, as owned engine-independent data. Everything here is what crosses the relay
// boundary; nothing here may reference engine memory in any form.

namespace RelayEvents
{
    // Emitted once when the MissionPlaying predicate goes false -> true: the engine has started
    // reporting a mission scene as playing. See MissionObserver for the predicate. Payload schema
    // version 1.
    constexpr const char* k_MissionPlaying = "mission.playing";
    constexpr int k_MissionPlayingSchemaVersion = 1;

    // Emitted once when the same predicate goes true -> false. It says only that: it carries no
    // claim about completion, success, failure, restart, abandonment, a menu transition or the
    // process ending (glacier-relay M2 design). Payload schema version 1, same shape as above.
    constexpr const char* k_MissionStopped = "mission.stopped";
    constexpr int k_MissionStoppedSchemaVersion = 1;
}

// The scene as observed on the frame an edge fired. Shared by both lifecycle events.
struct MissionScenePayload
{
    std::string scene_resource;
    std::string scene_type;
    std::string codename_hint;

    // Copied from the game's player registry when readable on the edge frame. Observation only:
    // not an identity, not a key, not used for deduplication (glacier-relay M1 design, section 3).
    std::optional<std::string> game_session_id;

    bool operator==(const MissionScenePayload&) const = default;
};

struct MissionPlayingEvent : MissionScenePayload
{
    bool operator==(const MissionPlayingEvent&) const = default;
};

struct MissionStoppedEvent : MissionScenePayload
{
    bool operator==(const MissionStoppedEvent&) const = default;
};

// What the mission observer can produce on one frame: at most one edge.
using MissionEvent = std::variant<MissionPlayingEvent, MissionStoppedEvent>;
