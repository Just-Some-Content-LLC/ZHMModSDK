#pragma once

#include <cstdint>
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

    // Contract lifecycle, normalized from Glacier's engine-authored telemetry (M2 B2, design
    // section 27). contract.started: Glacier recorded the start of a contract session (its
    // "ContractStart" event). contract.ended: Glacier recorded the end of one (its "ContractFailed"
    // event, which the engine raises for a manual restart and for an exit to the menu alike, so the
    // Relay name does not say "failed"). Neither name claims anything about the Relay mission
    // attempt, mission success, failure, completion or player death.
    constexpr const char* k_ContractStarted = "contract.started";
    constexpr int k_ContractStartedSchemaVersion = 1;
    constexpr const char* k_ContractEnded = "contract.ended";
    constexpr int k_ContractEndedSchemaVersion = 1;

    // Disguise, normalized from Glacier's engine-authored telemetry (M2 B3, design section 30).
    // disguise.equipped: Glacier asserted the player's worn outfit (definition id); kind "initial"
    // restates the outfit the attempt began in (its "StartingSuit" event, at intro end), kind
    // "change" says the worn outfit changed to this id (its "Disguise" event).
    // disguise.compromised: Glacier recorded that this outfit was blown ("DisguiseBlown").
    // disguise.compromise_cleared: Glacier recorded that it no longer is ("BrokenDisguiseCleared").
    // None of them says who noticed, whether a compromise persists across a later change, which NPC
    // or instance the outfit came from, or what the outfit is called; ids only.
    constexpr const char* k_DisguiseEquipped = "disguise.equipped";
    constexpr int k_DisguiseEquippedSchemaVersion = 1;
    constexpr const char* k_DisguiseCompromised = "disguise.compromised";
    constexpr int k_DisguiseCompromisedSchemaVersion = 1;
    constexpr const char* k_DisguiseCompromiseCleared = "disguise.compromise_cleared";
    constexpr int k_DisguiseCompromiseClearedSchemaVersion = 1;

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

// The start of a Glacier contract session, schema version 1. Bounded to contract lifecycle on
// purpose (M2 design, section 27.7): the loadout, item traits, game changers and spawn location
// the source event also carries are not part of this event; they belong to later vocabularies
// (items, B4) or to no decided use yet. contract_session_id is Glacier's identity for Glacier's
// session; it is not a Relay attempt identity and BEAM correlates the two by stream order only.
struct ContractStartedEvent
{
    std::string engine_event; // provenance: the Glacier event name ("ContractStart")
    std::string contract_session_id;
    std::string contract_id;
    std::string location_id;   // open-ended engine string, e.g. "LOCATION_PARIS"
    std::string contract_type; // open-ended engine string, e.g. "mission"
    int64_t difficulty_level = 0; // the engine's number, not mapped to a name
    std::string starting_disguise_repository_id;
    bool is_hitman_suit = false;
    std::optional<double> engine_timestamp_s; // the stream's Timestamp (seconds on the contract clock)

    bool operator==(const ContractStartedEvent&) const = default;
};

// The end of a Glacier contract session, schema version 1. reason is the engine's string verbatim;
// reason_kind is the Relay mapping of the strings observed so far ("restart", "exit_to_menu") with
// "other" for anything else, so a new engine string never makes a valid event malformed.
struct ContractEndedEvent
{
    std::string engine_event; // provenance: the Glacier event name ("ContractFailed")
    std::string contract_session_id;
    std::string contract_id;
    std::string reason;
    std::string reason_kind; // "restart" | "exit_to_menu" | "other"
    std::optional<double> engine_timestamp_s;

    bool operator==(const ContractEndedEvent&) const = default;
};

// One disguise occurrence, schema version 1 for each of the three Relay event types (M2 B3). The
// kind selects the event type; Initial and Change both publish as disguise.equipped and carry the
// Relay-owned "kind" on the wire, with engine_event kept as provenance. disguise_repository_id is
// the engine's outfit definition id verbatim: a definition, not an instance, and not a name.
struct DisguiseEvent
{
    enum class Kind
    {
        Initial,          // disguise.equipped, kind "initial"  (StartingSuit)
        Change,           // disguise.equipped, kind "change"   (Disguise)
        Compromised,      // disguise.compromised               (DisguiseBlown)
        CompromiseCleared // disguise.compromise_cleared        (BrokenDisguiseCleared)
    };

    Kind kind = Kind::Change;
    std::string engine_event; // provenance: the Glacier event name
    std::string disguise_repository_id;
    std::optional<std::string> contract_session_id;
    std::optional<double> engine_timestamp_s;

    bool operator==(const DisguiseEvent&) const = default;
};
