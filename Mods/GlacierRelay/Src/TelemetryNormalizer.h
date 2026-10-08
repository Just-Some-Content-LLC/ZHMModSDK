#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "RelayEvent.h"
#include "TelemetryObservation.h"

// The normalization boundary between Glacier's telemetry stream and Relay semantic events
// (ADR 0006; M2 design, Part B, section 17). Pure: no engine access, no I/O, no clock.
//
// It recognizes a bounded table of source event names, applies policy, validates the fields each
// Relay event needs, maps them into Relay-owned data, and reports everything else as counted
// outcomes. It never produces a generic pass-through of a source payload.
//
// Each table row also states how the plugin gates publication (M2 design, section 27.5):
// attempt-gated events (actor outcomes) are published only while the mission predicate is true;
// ungated events (contract lifecycle) are published whenever captured and valid, because Glacier
// emits them on both sides of the predicate's edges. The gate is a per-row fact, not a framework;
// no grace window, no native attachment of an event to an attempt.
class TelemetryNormalizer
{
public:
    enum class Outcome
    {
        Normalized,  // exactly one of the event members is set
        Unsupported, // source name not in the table; counted by name
        DontSend,    // the stream marked the event as not for transmission; policy: not normalized
        Malformed,   // supported name, but a required field is missing, mistyped or unreadable
    };

    enum class Gating
    {
        AttemptGated, // publish only while MissionObserver::Playing()
        Ungated,      // publish whenever captured and valid
    };

    struct Result
    {
        Outcome outcome = Outcome::Unsupported;
        Gating gating = Gating::AttemptGated;              // Normalized only
        std::optional<ActorOutcomeEvent> event;            // Normalized, attempt-gated
        std::optional<ContractStartedEvent> contract_started; // Normalized, ungated
        std::optional<ContractEndedEvent> contract_ended;     // Normalized, ungated
        std::optional<DisguiseEvent> disguise;                // Normalized, attempt-gated (M2 B3)
        std::string detail;                                // Malformed: the field and what was wrong
    };

    // Whether the intake should bother copying an event with this name at all (cheap name filter
    // for the Glacier-facing side). Policy flags are still checked by Normalize.
    static bool IsSupportedSourceName(std::string_view p_Name);

    Result Normalize(const TelemetryObservation& p_Observation);

    struct Counters
    {
        uint64_t normalized = 0;
        uint64_t unsupported = 0;
        uint64_t dont_send = 0;
        uint64_t malformed = 0;
        std::map<std::string, uint64_t> unsupported_by_name; // bounded in size
        std::map<std::string, uint64_t> malformed_by_name;
    };

    const Counters& GetCounters() const { return m_Counters; }

    // Relay-owned names for the engine's enums; "unknown" when the code is not in the known set.
    static std::string DeathTypeName(int p_Code);    // EDeathType
    static std::string DeathContextName(int p_Code); // EDeathContext
    static std::string ActorTypeName(int p_Code);    // EActorType

    // Relay's reading of a ContractFailed reason string: the two strings observed on this build map
    // to "restart" and "exit_to_menu"; anything else is "other" (the string itself is kept verbatim).
    static std::string ReasonKind(std::string_view p_Reason);

private:
    static constexpr size_t k_MaxCountedNames = 64;

    Result NormalizeActorOutcome(const TelemetryObservation& p_Observation, ActorOutcomeEvent::Kind p_Kind);
    Result NormalizeContractStarted(const TelemetryObservation& p_Observation);
    Result NormalizeContractEnded(const TelemetryObservation& p_Observation);
    Result NormalizeDisguise(const TelemetryObservation& p_Observation, DisguiseEvent::Kind p_Kind);
    Result Malformed(const TelemetryObservation& p_Observation, const std::string& p_Detail);
    void Count(std::map<std::string, uint64_t>& p_Map, const std::string& p_Name);

    Counters m_Counters;
};
