#pragma once

#include <optional>
#include <string>

// Semantic events, as owned engine-independent data. Everything here is what crosses the relay
// boundary; nothing here may reference engine memory in any form.

namespace RelayEvents
{
    // Emitted once when the engine starts reporting a mission scene as playing. See MissionObserver
    // for the predicate. Payload schema version 1.
    constexpr const char* k_MissionPlaying = "mission.playing";
    constexpr int k_MissionPlayingSchemaVersion = 1;
}

struct MissionPlayingEvent
{
    std::string scene_resource;
    std::string scene_type;
    std::string codename_hint;

    // Copied from the game's player registry when readable. Observation only: not an identity,
    // not a key, not used for deduplication (glacier-relay M1 design, section 3).
    std::optional<std::string> game_session_id;

    bool operator==(const MissionPlayingEvent&) const = default;
};
