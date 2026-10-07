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
class TelemetryNormalizer
{
public:
    enum class Outcome
    {
        Normalized,  // event is set
        Unsupported, // source name not in the table; counted by name
        DontSend,    // the stream marked the event as not for transmission; policy: not normalized
        Malformed,   // supported name, but a required field is missing, mistyped or unreadable
    };

    struct Result
    {
        Outcome outcome = Outcome::Unsupported;
        std::optional<ActorOutcomeEvent> event; // Normalized only
        std::string detail;                     // Malformed: the field and what was wrong
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

private:
    static constexpr size_t k_MaxCountedNames = 64;

    Result NormalizeActorOutcome(const TelemetryObservation& p_Observation, ActorOutcomeEvent::Kind p_Kind);
    void Count(std::map<std::string, uint64_t>& p_Map, const std::string& p_Name);

    Counters m_Counters;
};
