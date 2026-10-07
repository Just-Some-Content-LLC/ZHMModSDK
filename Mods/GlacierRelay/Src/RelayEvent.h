#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>

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

    // Actor outcomes, normalized from Glacier's engine-authored telemetry (ADR 0006; M2 design
    // Part B). actor.died: Glacier recorded a lethal outcome for an actor (its "Kill" event).
    // actor.pacified: Glacier recorded a non-lethal takedown (its "Pacify" event). Neither name
    // claims who or what caused it; the engine's own classification travels as fields.
    constexpr const char* k_ActorDied = "actor.died";
    constexpr int k_ActorDiedSchemaVersion = 1;
    constexpr const char* k_ActorPacified = "actor.pacified";
    constexpr int k_ActorPacifiedSchemaVersion = 1;

    // Provenance value carried by events normalized from the telemetry stream.
    constexpr const char* k_SourceEngineTelemetry = "engine_telemetry";
}

// The scene as observed on the frame an edge fired. Shared by both lifecycle events.
struct MissionScenePayload
{
    std::string scene_resource;
    std::string scene_type;
    std::string codename_hint;

    // Copied from the game's player registry when readable on the edge frame. B0 established that
    // this value is Glacier's ContractSessionId (M2 design, section 19); the v1 wire key stays
    // "game_session_id" for compatibility. Observation only: not a Relay identity, not a key, not
    // used for deduplication.
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

// One actor outcome, schema version 1. Every field is the engine's own observation or
// classification, carried without upgrade; the bounded subset of the stream's 33 fields that the
// M2 summary needs (M2 design, section 20).
struct ActorOutcomeEvent
{
    enum class Kind
    {
        Died,
        Pacified,
    };

    Kind kind = Kind::Died;

    // Actor observations. repository_id is the game's character definition id (shared by generic
    // NPCs); engine_actor_id is the stream's ActorId, observational only (B0 showed it is not a
    // stable actor identity). Neither is a Relay identity.
    std::string repository_id;
    std::string actor_name;
    uint64_t engine_actor_id = 0;
    std::string actor_type; // "civilian" | "guard" | "hitman" | "unknown"
    std::optional<int> actor_type_code; // set only when actor_type is "unknown"
    bool is_target = false;

    // The engine's classification of the outcome.
    std::string death_type;    // "pacify" | "kill" | "bloody_kill" | "unknown"  (EDeathType)
    std::optional<int> death_type_code;
    std::string death_context; // "undefined" | "not_hero" | "hidden" | "accident" | "murder" | "unknown" (EDeathContext)
    std::optional<int> death_context_code;
    bool accident = false;
    std::string kill_class;    // open-ended engine string, e.g. "melee", "ballistic", "explosion"
    std::string method_broad;  // open-ended engine string, e.g. "pistol", "unarmed", "accident"
    std::string method_strict; // open-ended engine string, often empty
    std::vector<std::string> damage_events; // open-ended engine strings, e.g. "Shoot", "Subdue"
    std::optional<std::string> item_repository_id;

    // Provenance: the Glacier contract session the stream attributed the event to, and the
    // stream's own timestamp (seconds since contract start). Observational.
    std::optional<std::string> contract_session_id;
    std::optional<double> engine_timestamp_s;

    bool operator==(const ActorOutcomeEvent&) const = default;
};
