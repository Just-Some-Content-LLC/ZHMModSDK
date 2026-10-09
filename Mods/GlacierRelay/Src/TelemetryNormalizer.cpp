#include "TelemetryNormalizer.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include <fmt/format.h>

namespace
{
    // The table. One row per supported source event; adding a vocabulary means adding rows and a
    // mapping function, never a pass-through. Each row names the Relay event family it maps to and
    // how the plugin gates its publication (TelemetryNormalizer.h).
    enum class Family
    {
        ActorDied,
        ActorPacified,
        ContractStarted,
        ContractEnded,
        DisguiseInitial,
        DisguiseChange,
        DisguiseCompromised,
        DisguiseCompromiseCleared,
        ItemPickedUp,
        ItemThrown,
        ItemRemovedFromInventory,
        ObjectiveCompleted,
    };

    struct SourceEvent
    {
        std::string_view name;
        Family family;
        TelemetryNormalizer::Gating gating;
    };

    // Disguise rows are attempt-gated: every observation of these names across B0, B1 and B2 fell
    // strictly inside the mission predicate window (design section 30.7), so the B1 rule applies and
    // the outside-attempt counter is the instrument that would reveal a surprise.
    constexpr SourceEvent k_Sources[] = {
        {"Kill", Family::ActorDied, TelemetryNormalizer::Gating::AttemptGated},
        {"Pacify", Family::ActorPacified, TelemetryNormalizer::Gating::AttemptGated},
        {"ContractStart", Family::ContractStarted, TelemetryNormalizer::Gating::Ungated},
        {"ContractFailed", Family::ContractEnded, TelemetryNormalizer::Gating::Ungated},
        {"StartingSuit", Family::DisguiseInitial, TelemetryNormalizer::Gating::AttemptGated},
        {"Disguise", Family::DisguiseChange, TelemetryNormalizer::Gating::AttemptGated},
        {"DisguiseBlown", Family::DisguiseCompromised, TelemetryNormalizer::Gating::AttemptGated},
        {"BrokenDisguiseCleared", Family::DisguiseCompromiseCleared, TelemetryNormalizer::Gating::AttemptGated},
        // Item rows (M2 B4, design section 38.5) are attempt-gated on the same evidence: all 24 B0
        // payloads and every name-only observation (B1, B3, section 36) fell strictly inside the
        // predicate window. ItemDropped and ItemDestroyed have no row: their payload shape and
        // subject were never captured (section 38.9); they stay counted as unsupported.
        {"ItemPickedUp", Family::ItemPickedUp, TelemetryNormalizer::Gating::AttemptGated},
        {"ItemThrown", Family::ItemThrown, TelemetryNormalizer::Gating::AttemptGated},
        {"ItemRemovedFromInventory", Family::ItemRemovedFromInventory, TelemetryNormalizer::Gating::AttemptGated},
        // The objective row (M2 B5, design section 42.4) is ungated: both captured occurrences sat
        // inside the predicate window, but what the engine emits at a completion transition —
        // possibly after the fall, when a gated row would keep only a name/index warning — has
        // never been observed. Publishing whenever captured preserves the occurrence; BEAM
        // attributes it, or keeps it unattributed, from its own evidence.
        {"ObjectiveCompleted", Family::ObjectiveCompleted, TelemetryNormalizer::Gating::Ungated},
    };

    const SourceEvent* FindSource(std::string_view p_Name)
    {
        for (const auto& s_Source : k_Sources)
            if (s_Source.name == p_Name)
                return &s_Source;

        return nullptr;
    }

    // The ContractFailed reason strings observed on game 3.280.0.0 (B0 and B1 corpora), matched
    // exactly. A variant spelling is evidence of a new string, not something to approximate.
    constexpr std::string_view k_ReasonRestart = "Contract ended manually: OnRestartLevel";
    constexpr std::string_view k_ReasonExitToMenu = "Contract ended manually: User pressed exit to Main menu";

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

    // Diagnostic description of a copied value for a malformed detail: its kind and, for an
    // Unsupported value, the engine type name the intake already copied (the only evidence of the
    // engine's type that reaches the normalizer). Bounded and escaped: the text goes to the durable
    // log on the frame thread and must never be trusted to be short or printable. No engine access.
    constexpr size_t k_MaxTypeNameBytes = 64;

    std::string EscapeBounded(std::string_view p_Text)
    {
        std::string s_Out;
        const size_t s_Limit = std::min(p_Text.size(), k_MaxTypeNameBytes);

        for (size_t i = 0; i < s_Limit; ++i)
        {
            const unsigned char c = static_cast<unsigned char>(p_Text[i]);

            if (c >= 0x20 && c < 0x7F && c != '\'' && c != '\\')
                s_Out += static_cast<char>(c);
            else
                s_Out += fmt::format("\\x{:02X}", c);
        }

        if (p_Text.size() > k_MaxTypeNameBytes)
            s_Out += "...";

        return s_Out;
    }

    std::string DescribeValue(const TelemetryValue& p_Value)
    {
        switch (p_Value.kind)
        {
            case TelemetryValue::Kind::Null: return "kind=Null";
            case TelemetryValue::Kind::Bool: return "kind=Bool";
            case TelemetryValue::Kind::Number: return "kind=Number";
            case TelemetryValue::Kind::String: return fmt::format("kind=String bytes={}", p_Value.text.size());
            case TelemetryValue::Kind::Array: return fmt::format("kind=Array items={}", p_Value.items.size());
            case TelemetryValue::Kind::Object: return fmt::format("kind=Object fields={}", p_Value.fields.size());
            case TelemetryValue::Kind::Unsupported: return fmt::format("kind=Unsupported type='{}'", EscapeBounded(p_Value.text));
        }

        return "kind=?";
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

    // Optional array of strings: absent is fine; present but not an array of strings is malformed.
    // The detail names the offending item's copied kind (bounded and escaped by DescribeValue).
    bool ReadOptionalStringArray(
        const TelemetryValue& p_Object, std::string_view p_Key, std::optional<std::vector<std::string>>& p_Out,
        std::string& p_Detail
    )
    {
        const auto* s_Value = p_Object.Find(p_Key);

        if (!s_Value)
        {
            p_Out.reset();
            return true;
        }

        if (s_Value->kind != TelemetryValue::Kind::Array)
        {
            p_Detail = fmt::format("field '{}' is not an array ({})", p_Key, DescribeValue(*s_Value));
            return false;
        }

        std::vector<std::string> s_Items;

        for (size_t i = 0; i < s_Value->items.size(); ++i)
        {
            const auto& s_Item = s_Value->items[i];

            if (s_Item.kind != TelemetryValue::Kind::String)
            {
                p_Detail = fmt::format("field '{}' has a non-string item (item {} {})", p_Key, i, DescribeValue(s_Item));
                return false;
            }

            s_Items.push_back(s_Item.text);
        }

        p_Out = std::move(s_Items);
        return true;
    }

    // Item rows (M2 B4): the keys the normalizer reads from the item object, in the order the
    // engine wrote them in B0. Category and ActionRewardType are deliberately not read (design
    // section 38.5) and so are not listed either.
    constexpr std::string_view k_ItemFields[] = {"RepositoryId", "InstanceId", "ItemName", "ItemType", "OnlineTraits"};

    // Objective row (M2 B5): the keys read from the objective object (design section 42.6).
    constexpr std::string_view k_ObjectiveFields[] = {"Id", "Type", "Category", "ExcludeFromScoring"};

    // The per-field diagnostic for a malformed object (design section 38.7): each expected
    // key's copied kind and, for an Unsupported value, the engine type name the intake copied.
    // Reveals the engine type only for values the intake could not read; a value read as
    // String/Number/Bool/Array/Object is reported by its Relay kind, which does not identify the
    // exact engine type. Bounded: a fixed list of keys, no string values (only byte counts), type
    // names escaped and capped by DescribeValue. No engine access, no new copy.
    template <size_t N>
    std::string DescribeFields(const TelemetryValue& p_Object, const std::string_view (&p_Keys)[N])
    {
        std::string s_Out = "fields:";

        for (size_t i = 0; i < N; ++i)
        {
            const auto* s_Value = p_Object.Find(p_Keys[i]);
            s_Out += fmt::format("{} '{}' {}", i ? "," : "", p_Keys[i], s_Value ? DescribeValue(*s_Value) : "absent");
        }

        return s_Out;
    }

    // Optional string with the described kind on failure (items and objectives).
    bool ReadOptionalStringDescribed(
        const TelemetryValue& p_Object, std::string_view p_Key, std::optional<std::string>& p_Out, std::string& p_Detail
    )
    {
        const auto* s_Field = p_Object.Find(p_Key);

        if (!s_Field)
        {
            p_Out.reset();
            return true;
        }

        if (s_Field->kind != TelemetryValue::Kind::String)
        {
            p_Detail = fmt::format("field '{}' is not a string ({})", p_Key, DescribeValue(*s_Field));
            return false;
        }

        p_Out = s_Field->text;
        return true;
    }

    // Optional bool: absent is fine; present must be a bool. false is a value, not absence.
    bool ReadOptionalBoolDescribed(
        const TelemetryValue& p_Object, std::string_view p_Key, std::optional<bool>& p_Out, std::string& p_Detail
    )
    {
        const auto* s_Field = p_Object.Find(p_Key);

        if (!s_Field)
        {
            p_Out.reset();
            return true;
        }

        if (s_Field->kind != TelemetryValue::Kind::Bool)
        {
            p_Detail = fmt::format("field '{}' is not a bool ({})", p_Key, DescribeValue(*s_Field));
            return false;
        }

        p_Out = s_Field->boolean;
        return true;
    }
}

bool TelemetryNormalizer::IsSupportedSourceName(std::string_view p_Name)
{
    return FindSource(p_Name) != nullptr;
}

std::string TelemetryNormalizer::ReasonKind(std::string_view p_Reason)
{
    if (p_Reason == k_ReasonRestart)
        return "restart";

    if (p_Reason == k_ReasonExitToMenu)
        return "exit_to_menu";

    return "other";
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

    const auto* s_Source = FindSource(p_Observation.name);

    if (!s_Source)
    {
        ++m_Counters.unsupported;
        Count(m_Counters.unsupported_by_name, p_Observation.name);
        s_Result.outcome = Outcome::Unsupported;
        return s_Result;
    }

    switch (s_Source->family)
    {
        case Family::ActorDied: s_Result = NormalizeActorOutcome(p_Observation, ActorOutcomeEvent::Kind::Died); break;
        case Family::ActorPacified: s_Result = NormalizeActorOutcome(p_Observation, ActorOutcomeEvent::Kind::Pacified); break;
        case Family::ContractStarted: s_Result = NormalizeContractStarted(p_Observation); break;
        case Family::ContractEnded: s_Result = NormalizeContractEnded(p_Observation); break;
        case Family::DisguiseInitial: s_Result = NormalizeDisguise(p_Observation, DisguiseEvent::Kind::Initial); break;
        case Family::DisguiseChange: s_Result = NormalizeDisguise(p_Observation, DisguiseEvent::Kind::Change); break;
        case Family::DisguiseCompromised: s_Result = NormalizeDisguise(p_Observation, DisguiseEvent::Kind::Compromised); break;
        case Family::DisguiseCompromiseCleared:
            s_Result = NormalizeDisguise(p_Observation, DisguiseEvent::Kind::CompromiseCleared);
            break;
        case Family::ItemPickedUp: s_Result = NormalizeItem(p_Observation, ItemEvent::Kind::PickedUp); break;
        case Family::ItemThrown: s_Result = NormalizeItem(p_Observation, ItemEvent::Kind::Thrown); break;
        case Family::ItemRemovedFromInventory:
            s_Result = NormalizeItem(p_Observation, ItemEvent::Kind::RemovedFromInventory);
            break;
        case Family::ObjectiveCompleted: s_Result = NormalizeObjective(p_Observation); break;
    }

    s_Result.gating = s_Source->gating;
    return s_Result;
}

TelemetryNormalizer::Result TelemetryNormalizer::Malformed(
    const TelemetryObservation& p_Observation, const std::string& p_Detail
)
{
    Result s_Result;
    ++m_Counters.malformed;
    Count(m_Counters.malformed_by_name, p_Observation.name);
    s_Result.outcome = Outcome::Malformed;
    s_Result.detail = p_Detail;
    return s_Result;
}

TelemetryNormalizer::Result TelemetryNormalizer::NormalizeContractStarted(const TelemetryObservation& p_Observation)
{
    std::string s_Detail;
    const TelemetryValue& s_Value = p_Observation.value;

    if (s_Value.kind != TelemetryValue::Kind::Object)
        return Malformed(p_Observation, "Value is not an object");

    // The session the event is about is on the stream envelope, not in Value; it is the subject of
    // the Relay event and therefore required (M2 design, section 27.7).
    if (p_Observation.contract_session_id.empty())
        return Malformed(p_Observation, "missing envelope field 'ContractSessionId'");

    ContractStartedEvent s_Event;
    s_Event.engine_event = p_Observation.name;
    s_Event.contract_session_id = p_Observation.contract_session_id;
    s_Event.contract_id = p_Observation.contract_id;

    if (!ReadString(s_Value, "LocationId", s_Event.location_id, s_Detail)
        || !ReadString(s_Value, "ContractType", s_Event.contract_type, s_Detail)
        || !ReadInteger(s_Value, "DifficultyLevel", s_Event.difficulty_level, s_Detail)
        || !ReadString(s_Value, "Disguise", s_Event.starting_disguise_repository_id, s_Detail)
        || !ReadBool(s_Value, "IsHitmanSuit", s_Event.is_hitman_suit, s_Detail))
    {
        return Malformed(p_Observation, s_Detail);
    }

    // Deliberately not read: Loadout, GameChangers, IsVR, SelectedCharacterId (section 27.7).

    if (p_Observation.has_timestamp)
        s_Event.engine_timestamp_s = p_Observation.timestamp_s;

    Result s_Result;
    ++m_Counters.normalized;
    s_Result.outcome = Outcome::Normalized;
    s_Result.contract_started = std::move(s_Event);
    return s_Result;
}

TelemetryNormalizer::Result TelemetryNormalizer::NormalizeContractEnded(const TelemetryObservation& p_Observation)
{
    const TelemetryValue& s_Value = p_Observation.value;

    // ContractFailed carries its reason as the Value itself, a string.
    if (s_Value.kind != TelemetryValue::Kind::String)
        return Malformed(p_Observation, "Value is not a string");

    if (s_Value.text.empty())
        return Malformed(p_Observation, "Value (reason) is empty");

    if (p_Observation.contract_session_id.empty())
        return Malformed(p_Observation, "missing envelope field 'ContractSessionId'");

    ContractEndedEvent s_Event;
    s_Event.engine_event = p_Observation.name;
    s_Event.contract_session_id = p_Observation.contract_session_id;
    s_Event.contract_id = p_Observation.contract_id;
    s_Event.reason = s_Value.text;
    s_Event.reason_kind = ReasonKind(s_Event.reason);

    if (p_Observation.has_timestamp)
        s_Event.engine_timestamp_s = p_Observation.timestamp_s;

    Result s_Result;
    ++m_Counters.normalized;
    s_Result.outcome = Outcome::Normalized;
    s_Result.contract_ended = std::move(s_Event);
    return s_Result;
}

TelemetryNormalizer::Result TelemetryNormalizer::NormalizeDisguise(
    const TelemetryObservation& p_Observation, DisguiseEvent::Kind p_Kind
)
{
    const TelemetryValue& s_Value = p_Observation.value;

    // All four source events carry the outfit definition repository id as the Value itself, a
    // string (B0 corpus, 8/8). It is the event's subject, so it is required and non-empty. Its
    // format is not checked: the engine's id form is evidence, not a contract.
    // The B3 run (design section 32) rejected 9/9 disguise values here; the copied kind and, for
    // an Unsupported value, the engine type name are the evidence the next step needs.
    if (s_Value.kind != TelemetryValue::Kind::String)
        return Malformed(p_Observation, fmt::format("Value is not a string ({})", DescribeValue(s_Value)));

    if (s_Value.text.empty())
        return Malformed(p_Observation, "Value (disguise repository id) is empty");

    DisguiseEvent s_Event;
    s_Event.kind = p_Kind;
    s_Event.engine_event = p_Observation.name;
    s_Event.disguise_repository_id = s_Value.text;

    // Provenance from the stream envelope, as on actor outcomes: optional, carried when present.
    if (!p_Observation.contract_session_id.empty())
        s_Event.contract_session_id = p_Observation.contract_session_id;

    if (p_Observation.has_timestamp)
        s_Event.engine_timestamp_s = p_Observation.timestamp_s;

    Result s_Result;
    ++m_Counters.normalized;
    s_Result.outcome = Outcome::Normalized;
    s_Result.disguise = std::move(s_Event);
    return s_Result;
}

TelemetryNormalizer::Result TelemetryNormalizer::NormalizeItem(
    const TelemetryObservation& p_Observation, ItemEvent::Kind p_Kind
)
{
    const TelemetryValue& s_Value = p_Observation.value;

    // All three source events carried the same seven-key object in B0 (24/24, design section
    // 38.1). No item payload has ever crossed the typed intake, so every rejection below carries
    // the per-field diagnostic of section 38.7: the copied kinds are the evidence the next step
    // needs, and they are the only evidence a rejection can give.
    if (s_Value.kind != TelemetryValue::Kind::Object)
        return Malformed(p_Observation, fmt::format("Value is not an object ({})", DescribeValue(s_Value)));

    auto s_Malformed = [&](const std::string& p_Detail) {
        return Malformed(p_Observation, fmt::format("{}; {}", p_Detail, DescribeFields(s_Value, k_ItemFields)));
    };

    // Required: the definition id, the event's subject. Non-empty; its format is not checked (the
    // engine's id form is evidence, not a contract), and it is accepted whether the intake copied
    // a ZString or rendered a ZRepositoryID (section 35).
    const auto* s_Id = s_Value.Find("RepositoryId");

    if (!s_Id)
        return s_Malformed("missing field 'RepositoryId'");

    if (s_Id->kind != TelemetryValue::Kind::String)
        return s_Malformed(fmt::format("field 'RepositoryId' is not a string ({})", DescribeValue(*s_Id)));

    if (s_Id->text.empty())
        return s_Malformed("field 'RepositoryId' is empty");

    ItemEvent s_Event;
    s_Event.kind = p_Kind;
    s_Event.engine_event = p_Observation.name;
    s_Event.item_repository_id = s_Id->text;

    // Optional strings: absent is fine; present must be a string. InstanceId is the one field
    // where an empty string is normal (24/24 sampled): it is carried only when non-empty, so the
    // Relay event never shows an instance the engine did not name.
    std::string s_Detail;

    if (!ReadOptionalStringDescribed(s_Value, "InstanceId", s_Event.item_instance_id, s_Detail)
        || !ReadOptionalStringDescribed(s_Value, "ItemName", s_Event.item_name, s_Detail)
        || !ReadOptionalStringDescribed(s_Value, "ItemType", s_Event.item_type, s_Detail)
        || !ReadOptionalStringArray(s_Value, "OnlineTraits", s_Event.online_traits, s_Detail))
    {
        return s_Malformed(s_Detail);
    }

    if (s_Event.item_instance_id && s_Event.item_instance_id->empty())
        s_Event.item_instance_id.reset();

    // Deliberately not read: Category (null 24/24, unknown type) and ActionRewardType (constant,
    // possibly an enum the intake copies as Unsupported). An unread field of any kind is harmless.

    // Provenance from the stream envelope, as on actor outcomes and disguise: optional.
    if (!p_Observation.contract_session_id.empty())
        s_Event.contract_session_id = p_Observation.contract_session_id;

    if (p_Observation.has_timestamp)
        s_Event.engine_timestamp_s = p_Observation.timestamp_s;

    Result s_Result;
    ++m_Counters.normalized;
    s_Result.outcome = Outcome::Normalized;
    s_Result.item = std::move(s_Event);
    return s_Result;
}

TelemetryNormalizer::Result TelemetryNormalizer::NormalizeObjective(const TelemetryObservation& p_Observation)
{
    const TelemetryValue& s_Value = p_Observation.value;

    // One recorded payload (B0, design section 42.1): {Id, Type, Category, ExcludeFromScoring}. Only
    // the subject is required; the other three are hypotheses from one sample and must produce a
    // malformed line with the per-field detail if the engine's types differ, not a silent drop.
    if (s_Value.kind != TelemetryValue::Kind::Object)
        return Malformed(p_Observation, fmt::format("Value is not an object ({})", DescribeValue(s_Value)));

    auto s_Malformed = [&](const std::string& p_Detail) {
        return Malformed(p_Observation, fmt::format("{}; {}", p_Detail, DescribeFields(s_Value, k_ObjectiveFields)));
    };

    const auto* s_Id = s_Value.Find("Id");

    if (!s_Id)
        return s_Malformed("missing field 'Id'");

    if (s_Id->kind != TelemetryValue::Kind::String)
        return s_Malformed(fmt::format("field 'Id' is not a string ({})", DescribeValue(*s_Id)));

    if (s_Id->text.empty())
        return s_Malformed("field 'Id' is empty");

    ObjectiveEvent s_Event;
    s_Event.engine_event = p_Observation.name;
    s_Event.objective_id = s_Id->text;

    std::string s_Detail;

    if (!ReadOptionalStringDescribed(s_Value, "Type", s_Event.objective_type, s_Detail)
        || !ReadOptionalStringDescribed(s_Value, "Category", s_Event.objective_category, s_Detail)
        || !ReadOptionalBoolDescribed(s_Value, "ExcludeFromScoring", s_Event.exclude_from_scoring, s_Detail))
    {
        return s_Malformed(s_Detail);
    }

    // Provenance from the stream envelope: the session is attribution evidence for BEAM, the
    // timestamp an observation; both optional. XboxGameMode/XboxDifficulty are not read.
    if (!p_Observation.contract_session_id.empty())
        s_Event.contract_session_id = p_Observation.contract_session_id;

    if (p_Observation.has_timestamp)
        s_Event.engine_timestamp_s = p_Observation.timestamp_s;

    Result s_Result;
    ++m_Counters.normalized;
    s_Result.outcome = Outcome::Normalized;
    s_Result.objective = std::move(s_Event);
    return s_Result;
}

TelemetryNormalizer::Result TelemetryNormalizer::NormalizeActorOutcome(
    const TelemetryObservation& p_Observation, ActorOutcomeEvent::Kind p_Kind
)
{
    Result s_Result;
    std::string s_Detail;

    auto s_Malformed = [&](const std::string& p_Detail) { return Malformed(p_Observation, p_Detail); };

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
