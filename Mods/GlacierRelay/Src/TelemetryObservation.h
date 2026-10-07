#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// One event of Glacier's engine-authored telemetry stream, copied into owned memory at the
// Glacier-facing boundary (TelemetryIntake). Everything here is plain values: after the detour
// returns, nothing in the relay holds a ZDynamicObject, a ZString view, a pointer or an SDK
// container (glacier-relay M2 design, Part B, section 17).
//
// TelemetryValue is a small owned tree with the shapes the stream uses (null, bool, number,
// string, array, object). Values the engine cannot be read as plain data are recorded as
// Unsupported with the engine type name, so a normalizer can reject them explicitly instead of
// guessing.
struct TelemetryValue
{
    enum class Kind
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
        Unsupported,
    };

    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text; // String: the value; Unsupported: the engine type name
    std::vector<TelemetryValue> items; // Array
    std::vector<std::pair<std::string, TelemetryValue>> fields; // Object, in source order

    // Object lookup; nullptr when this is not an object or the key is absent.
    const TelemetryValue* Find(std::string_view p_Key) const
    {
        if (kind != Kind::Object)
            return nullptr;

        for (const auto& s_Field : fields)
            if (s_Field.first == p_Key)
                return &s_Field.second;

        return nullptr;
    }

    bool operator==(const TelemetryValue&) const = default;
};

struct TelemetryObservation
{
    std::string name;                // the stream's event Name, e.g. "Kill"
    std::string contract_session_id; // empty when absent
    std::string contract_id;         // empty when absent
    double timestamp_s = 0.0;        // the stream's Timestamp (seconds since contract start); 0 when absent
    bool has_timestamp = false;
    bool dont_send = false;          // the stream's top-level "_DONTSEND" flag
    uint32_t event_index = 0;        // the manager's running index, for the native log only
    TelemetryValue value;            // the stream's Value

    bool operator==(const TelemetryObservation&) const = default;
};

// Limits applied while copying, so one event cannot make the intake do unbounded work on the
// game thread. Anything beyond them is truncated and the observation marked as such.
namespace TelemetryLimits
{
    constexpr size_t k_MaxDepth = 6;
    constexpr size_t k_MaxNodes = 512;
    constexpr size_t k_MaxStringBytes = 1024;
}
