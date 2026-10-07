#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "TelemetryObservation.h"

class ZDynamicObject;

// The Glacier-facing side of the telemetry boundary: the only code that reads a ZDynamicObject.
// Called from the ZAchievementManagerSimple::OnEventSent detour. It does the minimum on the game
// thread: read the event's Name and the _DONTSEND flag, decide whether the relay supports the
// name, and if so copy the bounded subset it needs into an owned TelemetryObservation. Nothing
// borrowed from the engine survives the call.
namespace TelemetryIntake
{
    enum class Decision
    {
        Captured,    // observation filled
        Unsupported, // name not supported by the normalizer; only counted
        DontSend,    // event carries _DONTSEND; only counted
        Unreadable,  // the event was not an object or had no readable Name
    };

    struct Inspection
    {
        Decision decision = Decision::Unreadable;
        std::string name;    // when readable
        bool truncated = false; // limits hit while copying (observation still usable but partial)
    };

    // Inspects the event and, when supported, fills p_Observation.
    Inspection Inspect(const ZDynamicObject& p_Event, uint32_t p_EventIndex, TelemetryObservation& p_Observation);

    // Copies any dynamic object into the owned tree, subject to TelemetryLimits. Exposed for the
    // raw diagnostic log setting; not used on the normal path.
    TelemetryValue CopyValue(const ZDynamicObject& p_Value, bool& p_Truncated);
}
