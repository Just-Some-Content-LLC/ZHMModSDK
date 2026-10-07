#include "TelemetryIntake.h"

#include <cstring>

#include "Glacier/Reflection.h"
#include "Glacier/TArray.h"
#include "Glacier/ZObject.h"
#include "Glacier/ZString.h"

#include "TelemetryNormalizer.h"

namespace
{
    // The engine's reflection type name for a value, or "" when it has none.
    std::string_view TypeNameOf(const ZObjectRef& p_Value)
    {
        const auto* s_Type = p_Value.GetTypeID();

        if (!s_Type || !s_Type->GetTypeInfo() || !s_Type->GetTypeInfo()->pszTypeName)
            return {};

        return s_Type->GetTypeInfo()->pszTypeName;
    }

    bool IsIntegerTypeName(std::string_view p_Name)
    {
        return p_Name == "int8" || p_Name == "uint8" || p_Name == "int16" || p_Name == "uint16" || p_Name == "int32"
            || p_Name == "uint32" || p_Name == "int64" || p_Name == "uint64";
    }

    double ReadInteger(std::string_view p_Name, const void* p_Data)
    {
        if (p_Name == "int8") return *static_cast<const int8_t*>(p_Data);
        if (p_Name == "uint8") return *static_cast<const uint8_t*>(p_Data);
        if (p_Name == "int16") return *static_cast<const int16_t*>(p_Data);
        if (p_Name == "uint16") return *static_cast<const uint16_t*>(p_Data);
        if (p_Name == "int32") return *static_cast<const int32_t*>(p_Data);
        if (p_Name == "uint32") return *static_cast<const uint32_t*>(p_Data);
        if (p_Name == "int64") return static_cast<double>(*static_cast<const int64_t*>(p_Data));
        return static_cast<double>(*static_cast<const uint64_t*>(p_Data));
    }

    std::string CopyString(const ZString& p_String, bool& p_Truncated)
    {
        const auto s_View = p_String.ToStringView();

        if (s_View.size() > TelemetryLimits::k_MaxStringBytes)
        {
            p_Truncated = true;
            return std::string(s_View.substr(0, TelemetryLimits::k_MaxStringBytes));
        }

        return std::string(s_View);
    }

    struct CopyBudget
    {
        size_t nodes = 0;
        bool truncated = false;
    };

    TelemetryValue Copy(const ZDynamicObject& p_Value, size_t p_Depth, CopyBudget& p_Budget)
    {
        TelemetryValue s_Out;

        if (++p_Budget.nodes > TelemetryLimits::k_MaxNodes || p_Depth > TelemetryLimits::k_MaxDepth)
        {
            p_Budget.truncated = true;
            s_Out.kind = TelemetryValue::Kind::Unsupported;
            s_Out.text = "<truncated>";
            return s_Out;
        }

        const auto s_TypeName = TypeNameOf(p_Value);
        const void* s_Data = p_Value.GetData();

        if (s_TypeName.empty() || !s_Data || p_Value.IsEmpty() || s_TypeName == "void")
        {
            s_Out.kind = TelemetryValue::Kind::Null;
            return s_Out;
        }

        if (s_TypeName == "ZString")
        {
            s_Out.kind = TelemetryValue::Kind::String;
            s_Out.text = CopyString(*static_cast<const ZString*>(s_Data), p_Budget.truncated);
            return s_Out;
        }

        if (s_TypeName == "bool")
        {
            s_Out.kind = TelemetryValue::Kind::Bool;
            s_Out.boolean = *static_cast<const bool*>(s_Data);
            return s_Out;
        }

        if (s_TypeName == "float64")
        {
            s_Out.kind = TelemetryValue::Kind::Number;
            s_Out.number = *static_cast<const double*>(s_Data);
            return s_Out;
        }

        if (s_TypeName == "float32")
        {
            s_Out.kind = TelemetryValue::Kind::Number;
            s_Out.number = *static_cast<const float*>(s_Data);
            return s_Out;
        }

        if (IsIntegerTypeName(s_TypeName))
        {
            s_Out.kind = TelemetryValue::Kind::Number;
            s_Out.number = ReadInteger(s_TypeName, s_Data);
            return s_Out;
        }

        if (p_Value.IsObject())
        {
            s_Out.kind = TelemetryValue::Kind::Object;
            const auto* s_Pairs = static_cast<const TArray<SDynamicObjectKeyValuePair>*>(s_Data);

            for (const auto& s_Pair : *s_Pairs)
                s_Out.fields.emplace_back(CopyString(s_Pair.sKey, p_Budget.truncated), Copy(s_Pair.value, p_Depth + 1, p_Budget));

            return s_Out;
        }

        if (p_Value.IsArray())
        {
            s_Out.kind = TelemetryValue::Kind::Array;
            const auto* s_Items = static_cast<const TArray<ZDynamicObject>*>(s_Data);

            for (const auto& s_Item : *s_Items)
                s_Out.items.push_back(Copy(s_Item, p_Depth + 1, p_Budget));

            return s_Out;
        }

        if (s_TypeName == "TArray<ZString>")
        {
            s_Out.kind = TelemetryValue::Kind::Array;
            const auto* s_Items = static_cast<const TArray<ZString>*>(s_Data);

            for (const auto& s_Item : *s_Items)
            {
                TelemetryValue s_Text;
                s_Text.kind = TelemetryValue::Kind::String;
                s_Text.text = CopyString(s_Item, p_Budget.truncated);
                s_Out.items.push_back(std::move(s_Text));
            }

            return s_Out;
        }

        // Anything else (SVector3, enums, resource ids, ...) is recorded by type name so a
        // normalizer that needs it fails loudly instead of guessing.
        s_Out.kind = TelemetryValue::Kind::Unsupported;
        s_Out.text = std::string(s_TypeName);
        return s_Out;
    }

    // Looks up one top-level key without copying the whole object.
    const ZDynamicObject* TopLevel(const TArray<SDynamicObjectKeyValuePair>& p_Pairs, std::string_view p_Key)
    {
        for (const auto& s_Pair : p_Pairs)
            if (s_Pair.sKey.ToStringView() == p_Key)
                return &s_Pair.value;

        return nullptr;
    }
}

TelemetryValue TelemetryIntake::CopyValue(const ZDynamicObject& p_Value, bool& p_Truncated)
{
    CopyBudget s_Budget;
    TelemetryValue s_Value = Copy(p_Value, 0, s_Budget);
    p_Truncated = s_Budget.truncated;
    return s_Value;
}

TelemetryIntake::Inspection TelemetryIntake::Inspect(
    const ZDynamicObject& p_Event, uint32_t p_EventIndex, TelemetryObservation& p_Observation
)
{
    Inspection s_Inspection;

    if (!p_Event.IsObject() || !p_Event.GetData())
        return s_Inspection;

    const auto& s_Pairs = *static_cast<const TArray<SDynamicObjectKeyValuePair>*>(p_Event.GetData());

    const auto* s_Name = TopLevel(s_Pairs, "Name");

    if (!s_Name || TypeNameOf(*s_Name) != "ZString" || !s_Name->GetData())
        return s_Inspection;

    bool s_Truncated = false;
    s_Inspection.name = CopyString(*static_cast<const ZString*>(s_Name->GetData()), s_Truncated);

    // Policy metadata first: the flag decides regardless of the name (M2 design, section 18).
    if (const auto* s_DontSend = TopLevel(s_Pairs, "_DONTSEND"))
    {
        if (TypeNameOf(*s_DontSend) == "bool" && s_DontSend->GetData() && *static_cast<const bool*>(s_DontSend->GetData()))
        {
            s_Inspection.decision = Decision::DontSend;
            return s_Inspection;
        }
    }

    if (!TelemetryNormalizer::IsSupportedSourceName(s_Inspection.name))
    {
        s_Inspection.decision = Decision::Unsupported;
        return s_Inspection;
    }

    // Supported: copy the envelope fields the normalizer uses and the Value subtree, bounded.
    p_Observation = TelemetryObservation{};
    p_Observation.name = s_Inspection.name;
    p_Observation.event_index = p_EventIndex;

    if (const auto* s_Session = TopLevel(s_Pairs, "ContractSessionId"); s_Session && TypeNameOf(*s_Session) == "ZString" && s_Session->GetData())
        p_Observation.contract_session_id = CopyString(*static_cast<const ZString*>(s_Session->GetData()), s_Truncated);

    if (const auto* s_Contract = TopLevel(s_Pairs, "ContractId"); s_Contract && TypeNameOf(*s_Contract) == "ZString" && s_Contract->GetData())
        p_Observation.contract_id = CopyString(*static_cast<const ZString*>(s_Contract->GetData()), s_Truncated);

    if (const auto* s_Timestamp = TopLevel(s_Pairs, "Timestamp"))
    {
        const auto s_Type = TypeNameOf(*s_Timestamp);

        if (s_Timestamp->GetData() && (s_Type == "float64" || s_Type == "float32" || IsIntegerTypeName(s_Type)))
        {
            CopyBudget s_Budget;
            p_Observation.timestamp_s = Copy(*s_Timestamp, 0, s_Budget).number;
            p_Observation.has_timestamp = true;
        }
    }

    CopyBudget s_Budget;

    if (const auto* s_Value = TopLevel(s_Pairs, "Value"))
        p_Observation.value = Copy(*s_Value, 0, s_Budget);

    s_Inspection.truncated = s_Truncated || s_Budget.truncated;
    s_Inspection.decision = Decision::Captured;
    return s_Inspection;
}
