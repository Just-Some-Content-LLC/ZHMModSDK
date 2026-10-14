#include "OutfitObservation.h"

namespace OutfitObservation
{
    namespace
    {
        using CommandProtocol::ObservedValue;

        ObservedValue Base(FieldStatus p_Status, const std::string& p_Reason)
        {
            ObservedValue s_Value;
            s_Value.field_status = p_Status;
            s_Value.field_reason = p_Reason;
            s_Value.kind = ObservedValue::Kind::Null;
            return s_Value;
        }
    }

    void Append(Entries& p_Out, const char* p_Name, const Field<std::string>& p_Field)
    {
        ObservedValue s_Value = Base(p_Field.status, p_Field.reason);
        if (p_Field.observed())
        {
            s_Value.kind = ObservedValue::Kind::Text;
            s_Value.text = *p_Field.value;
        }
        p_Out.emplace_back(p_Name, std::move(s_Value));
    }

    void Append(Entries& p_Out, const char* p_Name, const Field<int64_t>& p_Field)
    {
        ObservedValue s_Value = Base(p_Field.status, p_Field.reason);
        if (p_Field.observed())
        {
            s_Value.kind = ObservedValue::Kind::Integer;
            s_Value.integer = *p_Field.value;
        }
        p_Out.emplace_back(p_Name, std::move(s_Value));
    }

    void Append(Entries& p_Out, const char* p_Name, const Field<bool>& p_Field)
    {
        ObservedValue s_Value = Base(p_Field.status, p_Field.reason);
        if (p_Field.observed())
        {
            s_Value.kind = ObservedValue::Kind::Bool;
            s_Value.boolean = *p_Field.value;
        }
        p_Out.emplace_back(p_Name, std::move(s_Value));
    }

    void AppendStatus(Entries& p_Out, const char* p_Name, FieldStatus p_Status, const std::string& p_Reason)
    {
        p_Out.emplace_back(p_Name, Base(p_Status, p_Reason));
    }

    void DescribeHeroOutfit(const KitShape& p_Kit, int64_t p_Charset, int64_t p_Variation, HeroOutfit& p_Out)
    {
        p_Out.outfit_id = Field<std::string>::Observed(p_Kit.id);
        p_Out.charset = Field<int64_t>::Observed(p_Charset);
        p_Out.variation = Field<int64_t>::Observed(p_Variation);
        p_Out.charset_count = Field<int64_t>::Observed(static_cast<int64_t>(p_Kit.heroa_variations.size()));
        p_Out.is_hitman_suit = Field<bool>::Observed(p_Kit.is_hitman_suit);

        if (p_Charset < 0 || static_cast<size_t>(p_Charset) >= p_Kit.heroa_variations.size())
        {
            p_Out.heroa_variations = Field<int64_t>::Of(FieldStatus::Unavailable, "charset index out of bounds");
            p_Out.heroa_valid = Field<bool>::Observed(false);
            return;
        }

        p_Out.heroa_variations = p_Kit.heroa_variations[static_cast<size_t>(p_Charset)];
        p_Out.heroa_valid = Field<bool>::Observed(
            p_Out.heroa_variations.observed() && p_Variation >= 0 && p_Variation < *p_Out.heroa_variations.value
        );
    }

    void DescribeCandidate(const KitShape& p_Kit, KitResolution& p_Out)
    {
        p_Out.found = Field<bool>::Observed(true);
        p_Out.charsets = Field<int64_t>::Observed(static_cast<int64_t>(p_Kit.heroa_variations.size()));
        p_Out.is_hitman_suit = Field<bool>::Observed(p_Kit.is_hitman_suit);

        if (p_Kit.heroa_variations.empty())
            p_Out.heroa_variations = Field<int64_t>::Of(FieldStatus::Unavailable, "kit has no charset");
        else
            p_Out.heroa_variations = p_Kit.heroa_variations[0];

        p_Out.heroa_usable = Field<bool>::Observed(p_Out.heroa_variations.observed() && *p_Out.heroa_variations.value > 0);
    }
}
