#pragma once

#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>

#include "TelemetryObservation.h"

// A small JSON reader for tests and the wire probe only: it turns recorded telemetry (the B0
// corpus) into the TelemetryValue tree the Glacier-facing intake would have produced, so the
// engine-independent layers can be exercised without the game. Not part of the adapter; the
// adapter has no parser.
namespace TestJson
{
    class Parser
    {
    public:
        explicit Parser(std::string_view p_Text) : m_Text(p_Text) {}

        TelemetryValue Parse()
        {
            TelemetryValue s_Value = ParseValue();
            SkipSpace();
            if (m_Pos != m_Text.size())
                throw std::runtime_error("trailing characters");
            return s_Value;
        }

    private:
        void SkipSpace()
        {
            while (m_Pos < m_Text.size() && std::isspace(static_cast<unsigned char>(m_Text[m_Pos])))
                ++m_Pos;
        }

        bool Consume(char p_Char)
        {
            SkipSpace();
            if (m_Pos < m_Text.size() && m_Text[m_Pos] == p_Char)
            {
                ++m_Pos;
                return true;
            }
            return false;
        }

        void Expect(char p_Char)
        {
            if (!Consume(p_Char))
                throw std::runtime_error(std::string("expected '") + p_Char + "'");
        }

        TelemetryValue ParseValue()
        {
            SkipSpace();
            if (m_Pos >= m_Text.size())
                throw std::runtime_error("unexpected end");

            const char s_Char = m_Text[m_Pos];
            TelemetryValue s_Value;

            if (s_Char == '{')
            {
                ++m_Pos;
                s_Value.kind = TelemetryValue::Kind::Object;
                if (Consume('}'))
                    return s_Value;
                do
                {
                    std::string s_Key = ParseString();
                    Expect(':');
                    s_Value.fields.emplace_back(std::move(s_Key), ParseValue());
                } while (Consume(','));
                Expect('}');
                return s_Value;
            }

            if (s_Char == '[')
            {
                ++m_Pos;
                s_Value.kind = TelemetryValue::Kind::Array;
                if (Consume(']'))
                    return s_Value;
                do
                {
                    s_Value.items.push_back(ParseValue());
                } while (Consume(','));
                Expect(']');
                return s_Value;
            }

            if (s_Char == '"')
            {
                s_Value.kind = TelemetryValue::Kind::String;
                s_Value.text = ParseString();
                return s_Value;
            }

            if (m_Text.compare(m_Pos, 4, "true") == 0)
            {
                m_Pos += 4;
                s_Value.kind = TelemetryValue::Kind::Bool;
                s_Value.boolean = true;
                return s_Value;
            }

            if (m_Text.compare(m_Pos, 5, "false") == 0)
            {
                m_Pos += 5;
                s_Value.kind = TelemetryValue::Kind::Bool;
                return s_Value;
            }

            if (m_Text.compare(m_Pos, 4, "null") == 0)
            {
                m_Pos += 4;
                return s_Value;
            }

            // Number.
            const size_t s_Start = m_Pos;
            while (m_Pos < m_Text.size() && (std::isdigit(static_cast<unsigned char>(m_Text[m_Pos])) || m_Text[m_Pos] == '-'
                   || m_Text[m_Pos] == '+' || m_Text[m_Pos] == '.' || m_Text[m_Pos] == 'e' || m_Text[m_Pos] == 'E'))
                ++m_Pos;
            if (s_Start == m_Pos)
                throw std::runtime_error("unexpected character");
            s_Value.kind = TelemetryValue::Kind::Number;
            s_Value.number = std::strtod(std::string(m_Text.substr(s_Start, m_Pos - s_Start)).c_str(), nullptr);
            return s_Value;
        }

        std::string ParseString()
        {
            Expect('"');
            std::string s_Out;
            while (m_Pos < m_Text.size())
            {
                const char s_Char = m_Text[m_Pos++];
                if (s_Char == '"')
                    return s_Out;
                if (s_Char == '\\')
                {
                    if (m_Pos >= m_Text.size())
                        break;
                    const char s_Escaped = m_Text[m_Pos++];
                    switch (s_Escaped)
                    {
                        case 'n': s_Out += '\n'; break;
                        case 't': s_Out += '\t'; break;
                        case 'r': s_Out += '\r'; break;
                        case 'b': s_Out += '\b'; break;
                        case 'f': s_Out += '\f'; break;
                        case 'u':
                        {
                            const unsigned s_Code = std::strtoul(std::string(m_Text.substr(m_Pos, 4)).c_str(), nullptr, 16);
                            m_Pos += 4;
                            if (s_Code < 0x80) s_Out += static_cast<char>(s_Code);
                            else if (s_Code < 0x800) { s_Out += static_cast<char>(0xC0 | (s_Code >> 6)); s_Out += static_cast<char>(0x80 | (s_Code & 0x3F)); }
                            else { s_Out += static_cast<char>(0xE0 | (s_Code >> 12)); s_Out += static_cast<char>(0x80 | ((s_Code >> 6) & 0x3F)); s_Out += static_cast<char>(0x80 | (s_Code & 0x3F)); }
                            break;
                        }
                        default: s_Out += s_Escaped; break;
                    }
                    continue;
                }
                s_Out += s_Char;
            }
            throw std::runtime_error("unterminated string");
        }

        std::string_view m_Text;
        size_t m_Pos = 0;
    };

    inline TelemetryValue Parse(std::string_view p_Text) { return Parser(p_Text).Parse(); }

    // Builds the observation the intake would have produced from one recorded stream event.
    inline TelemetryObservation ObservationFromRecordedEvent(std::string_view p_Json, uint32_t p_EventIndex = 0)
    {
        const TelemetryValue s_Root = Parse(p_Json);
        TelemetryObservation s_Observation;
        s_Observation.event_index = p_EventIndex;

        if (const auto* s_Name = s_Root.Find("Name"); s_Name && s_Name->kind == TelemetryValue::Kind::String)
            s_Observation.name = s_Name->text;
        if (const auto* s_Session = s_Root.Find("ContractSessionId"); s_Session && s_Session->kind == TelemetryValue::Kind::String)
            s_Observation.contract_session_id = s_Session->text;
        if (const auto* s_Contract = s_Root.Find("ContractId"); s_Contract && s_Contract->kind == TelemetryValue::Kind::String)
            s_Observation.contract_id = s_Contract->text;
        if (const auto* s_Timestamp = s_Root.Find("Timestamp"); s_Timestamp && s_Timestamp->kind == TelemetryValue::Kind::Number)
        {
            s_Observation.timestamp_s = s_Timestamp->number;
            s_Observation.has_timestamp = true;
        }
        if (const auto* s_DontSend = s_Root.Find("_DONTSEND"); s_DontSend && s_DontSend->kind == TelemetryValue::Kind::Bool)
            s_Observation.dont_send = s_DontSend->boolean;
        if (const auto* s_Value = s_Root.Find("Value"))
            s_Observation.value = *s_Value;

        return s_Observation;
    }
}
