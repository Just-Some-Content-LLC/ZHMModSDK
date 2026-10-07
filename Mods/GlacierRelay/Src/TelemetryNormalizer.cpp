#include "TelemetryNormalizer.h"

#include <cmath>

#include <fmt/format.h>

namespace
{
    // The table. One row per supported source event; adding a vocabulary means adding rows and a
    // mapping function, never a pass-through.
    struct SourceEvent
    {
        std::string_view name;
        ActorOutcomeEvent::Kind kind;
    };

    constexpr SourceEvent k_ActorOutcomeSources[] = {
        {"Kill", ActorOutcomeEvent::Kind::Died},
        {"Pacify", ActorOutcomeEvent::Kind::Pacified},
    };

    const SourceEvent* FindActorOutcomeSource(std::string_view p_Name)
    {
        for (const auto& s_Source : k_ActorOutcomeSources)
            if (s_Source.name == p_Name)
                return &s_Source;

        return nullptr;
    }

    // Field readers. Each returns false and fills p_Detail when the field is missing or not of the
    // expected kind. The stream sends every number as a float64, so integers are accepted when the
    // number is integral.
    bool ReadString(const TelemetryValue& p_Object, std::string_view p_Key, std::string& p_Out, std::string& p_Detail)
    {
        const auto* s_Value = p_Object.Find(p_Key);

        if (!s_Value)
        {
            p_Detail = fmt::format("missing field '{}'", p_Key);
            return false;
        }

        if (s_Value->kind != TelemetryValue::Kind::String)
        {
            p_Detail = fmt::format("field '{}' is not a string", p_Key);
            return false;
        }

        p_Out = s_Value->text;
        return true;
    }

    bool ReadBool(const TelemetryValue& p_Object, std::string_view p_Key, bool& p_Out, std::string& p_Detail)
    {
        const auto* s_Value = p_Object.Find(p_Key);

        if (!s_Value)
        {
            p_Detail = fmt::format("missing field '{}'", p_Key);
            return false;
        }

        if (s_Value->kind != TelemetryValue::Kind::Bool)
        {
            p_Detail = fmt::format("field '{}' is not a bool", p_Key);
            return false;
        }

        p_Out = s_Value->boolean;
        return true;
    }

    bool ReadInteger(const TelemetryValue& p_Object, std::string_view p_Key, int64_t& p_Out, std::string& p_Detail)
    {
        const auto* s_Value = p_Object.Find(p_Key);

        if (!s_Value)
        {
            p_Detail = fmt::format("missing field '{}'", p_Key);
            return false;
        }

        if (s_Value->kind != TelemetryValue::Kind::Number || !std::isfinite(s_Value->number)
            || std::floor(s_Value->number) != s_Value->number)
        {
            p_Detail = fmt::format("field '{}' is not an integral number", p_Key);
            return false;
        }

        p_Out = static_cast<int64_t>(s_Value->number);
        return true;
    }

    bool ReadStringArray(
        const TelemetryValue& p_Object, std::string_view p_Key, std::vector<std::string>& p_Out, std::string& p_Detail
    )
    {
        const auto* s_Value = p_Object.Find(p_Key);

        if (!s_Value)
        {
            p_Detail = fmt::format("missing field '{}'", p_Key);
            return false;
        }

        if (s_Value->kind != TelemetryValue::Kind::Array)
        {
            p_Detail = fmt::format("field '{}' is not an array", p_Key);
            return false;
        }

        p_Out.clear();

        for (const auto& s_Item : s_Value->items)
        {
            if (s_Item.kind != TelemetryValue::Kind::String)
            {
                p_Detail = fmt::format("field '{}' has a non-string item", p_Key);
                return false;
            }

            p_Out.push_back(s_Item.text);
        }

        return true;
    }

    // Optional string: absent is fine; present but not a string is malformed.
    bool ReadOptionalString(
        const TelemetryValue& p_Object, std::string_view p_Key, std::optional<std::string>& p_Out, std::string& p_Detail
    )
    {
        const auto* s_Value = p_Object.Find(p_Key);

        if (!s_Value)
        {
            p_Out.reset();
            return true;
        }

        if (s_Value->kind != TelemetryValue::Kind::String)
        {
            p_Detail = fmt::format("field '{}' is not a string", p_Key);
            return false;
        }

        p_Out = s_Value->text;
        return true;
    }
}

bool TelemetryNormalizer::IsSupportedSourceName(std::string_view p_Name)
{
    return FindActorOutcomeSource(p_Name) != nullptr;
}

std::string TelemetryNormalizer::DeathTypeName(int p_Code)
{
    switch (p_Code) // EDeathType
    {
        case 3: return "pacify";
        case 4: return "kill";
        case 5: return "bloody_kill";
        default: return "unknown";
    }
}

std::string TelemetryNormalizer::DeathContextName(int p_Code)
{
    switch (p_Code) // EDeathContext
    {
        case 0: return "undefined";
        case 1: return "not_hero";
        case 2: return "hidden";
        case 3: return "accident";
        case 4: return "murder";
        default: return "unknown";
    }
}

std::string TelemetryNormalizer::ActorTypeName(int p_Code)
{
    switch (p_Code) // EActorType
    {
        case 0: return "civilian";
        case 1: return "guard";
        case 2: return "hitman";
        default: return "unknown";
    }
}

void TelemetryNormalizer::Count(std::map<std::string, uint64_t>& p_Map, const std::string& p_Name)
{
    if (p_Map.size() >= k_MaxCountedNames && p_Map.find(p_Name) == p_Map.end())
    {
        ++p_Map["<other>"];
        return;
    }

    ++p_Map[p_Name];
}

TelemetryNormalizer::Result TelemetryNormalizer::Normalize(const TelemetryObservation& p_Observation)
{
    Result s_Result;

    // Policy first: an event the engine marked as not for transmission is not normalized, whatever
    // its name (M2 design, section 18).
    if (p_Observation.dont_send)
    {
        ++m_Counters.dont_send;
        s_Result.outcome = Outcome::DontSend;
        return s_Result;
    }

    const auto* s_Source = FindActorOutcomeSource(p_Observation.name);

    if (!s_Source)
    {
        ++m_Counters.unsupported;
        Count(m_Counters.unsupported_by_name, p_Observation.name);
        s_Result.outcome = Outcome::Unsupported;
        return s_Result;
    }

    return NormalizeActorOutcome(p_Observation, s_Source->kind);
}

TelemetryNormalizer::Result TelemetryNormalizer::NormalizeActorOutcome(
    const TelemetryObservation& p_Observation, ActorOutcomeEvent::Kind p_Kind
)
{
    Result s_Result;
    std::string s_Detail;

    auto s_Malformed = [&](const std::string& p_Detail) {
        ++m_Counters.malformed;
        Count(m_Counters.malformed_by_name, p_Observation.name);
        s_Result.outcome = Outcome::Malformed;
        s_Result.detail = p_Detail;
        return s_Result;
    };

    const TelemetryValue& s_Value = p_Observation.value;

    if (s_Value.kind != TelemetryValue::Kind::Object)
        return s_Malformed("Value is not an object");

    ActorOutcomeEvent s_Event;
    s_Event.kind = p_Kind;

    int64_t s_ActorId = 0, s_ActorType = 0, s_DeathType = 0, s_DeathContext = 0;

    // Required fields: the bounded subset the M2 summary needs (M2 design, section 20).
    if (!ReadString(s_Value, "RepositoryId", s_Event.repository_id, s_Detail)
        || !ReadString(s_Value, "ActorName", s_Event.actor_name, s_Detail)
        || !ReadInteger(s_Value, "ActorId", s_ActorId, s_Detail)
        || !ReadInteger(s_Value, "ActorType", s_ActorType, s_Detail)
        || !ReadBool(s_Value, "IsTarget", s_Event.is_target, s_Detail)
        || !ReadInteger(s_Value, "KillType", s_DeathType, s_Detail)
        || !ReadInteger(s_Value, "KillContext", s_DeathContext, s_Detail)
        || !ReadBool(s_Value, "Accident", s_Event.accident, s_Detail)
        || !ReadString(s_Value, "KillClass", s_Event.kill_class, s_Detail)
        || !ReadString(s_Value, "KillMethodBroad", s_Event.method_broad, s_Detail)
        || !ReadString(s_Value, "KillMethodStrict", s_Event.method_strict, s_Detail)
        || !ReadStringArray(s_Value, "DamageEvents", s_Event.damage_events, s_Detail)
        || !ReadOptionalString(s_Value, "KillItemRepositoryId", s_Event.item_repository_id, s_Detail))
    {
        return s_Malformed(s_Detail);
    }

    if (s_Event.repository_id.empty())
        return s_Malformed("field 'RepositoryId' is empty");

    if (s_ActorId < 0 || s_ActorId > static_cast<int64_t>(UINT32_MAX))
        return s_Malformed("field 'ActorId' is out of range");

    s_Event.engine_actor_id = static_cast<uint64_t>(s_ActorId);

    // Enum codes become Relay names; an unknown code keeps its number beside "unknown" so the
    // evidence survives without the relay inventing a meaning.
    s_Event.actor_type = ActorTypeName(static_cast<int>(s_ActorType));
    if (s_Event.actor_type == "unknown")
        s_Event.actor_type_code = static_cast<int>(s_ActorType);

    s_Event.death_type = DeathTypeName(static_cast<int>(s_DeathType));
    if (s_Event.death_type == "unknown")
        s_Event.death_type_code = static_cast<int>(s_DeathType);

    s_Event.death_context = DeathContextName(static_cast<int>(s_DeathContext));
    if (s_Event.death_context == "unknown")
        s_Event.death_context_code = static_cast<int>(s_DeathContext);

    // Provenance from the stream envelope.
    if (!p_Observation.contract_session_id.empty())
        s_Event.contract_session_id = p_Observation.contract_session_id;

    if (p_Observation.has_timestamp)
        s_Event.engine_timestamp_s = p_Observation.timestamp_s;

    ++m_Counters.normalized;
    s_Result.outcome = Outcome::Normalized;
    s_Result.event = std::move(s_Event);
    return s_Result;
}
