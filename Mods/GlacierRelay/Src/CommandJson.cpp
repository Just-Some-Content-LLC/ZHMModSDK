#include "CommandJson.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace CommandJson
{
    const Value* Value::Find(std::string_view p_Key) const
    {
        if (kind != Kind::Object)
            return nullptr;

        for (const auto& s_Field : fields)
            if (s_Field.first == p_Key)
                return &s_Field.second;

        return nullptr;
    }

    bool ValidUtf8(std::string_view p_Bytes)
    {
        size_t i = 0;
        const size_t n = p_Bytes.size();

        while (i < n)
        {
            const unsigned char c = static_cast<unsigned char>(p_Bytes[i]);

            if (c < 0x80)
            {
                ++i;
                continue;
            }

            size_t s_Length;
            uint32_t s_Code;
            uint32_t s_Min;

            if ((c & 0xE0) == 0xC0) { s_Length = 2; s_Code = c & 0x1F; s_Min = 0x80; }
            else if ((c & 0xF0) == 0xE0) { s_Length = 3; s_Code = c & 0x0F; s_Min = 0x800; }
            else if ((c & 0xF8) == 0xF0) { s_Length = 4; s_Code = c & 0x07; s_Min = 0x10000; }
            else return false;

            if (i + s_Length > n)
                return false;

            for (size_t k = 1; k < s_Length; ++k)
            {
                const unsigned char s_Cont = static_cast<unsigned char>(p_Bytes[i + k]);
                if ((s_Cont & 0xC0) != 0x80)
                    return false;
                s_Code = (s_Code << 6) | (s_Cont & 0x3F);
            }

            if (s_Code < s_Min || s_Code > 0x10FFFF || (s_Code >= 0xD800 && s_Code <= 0xDFFF))
                return false;

            i += s_Length;
        }

        return true;
    }

    namespace
    {
        class Reader
        {
        public:
            explicit Reader(std::string_view p_Text) : m_Text(p_Text) {}

            ParseResult Run()
            {
                ParseResult s_Result;

                if (!ValidUtf8(m_Text))
                    return Fail("invalid utf-8");

                if (!ParseValue(s_Result.value, 0))
                    return Fail(m_Error);

                SkipSpace();

                if (m_Pos != m_Text.size())
                    return Fail("trailing characters");

                s_Result.ok = true;
                return s_Result;
            }

        private:
            ParseResult Fail(const std::string& p_Error)
            {
                ParseResult s_Result;
                s_Result.ok = false;
                s_Result.error = p_Error;
                return s_Result;
            }

            bool Error(const char* p_Error)
            {
                if (m_Error.empty())
                    m_Error = p_Error;
                return false;
            }

            void SkipSpace()
            {
                while (m_Pos < m_Text.size())
                {
                    const char c = m_Text[m_Pos];
                    if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                        ++m_Pos;
                    else
                        break;
                }
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

            bool Count()
            {
                if (++m_Nodes > k_MaxNodes)
                    return Error("too many values");
                return true;
            }

            bool ParseValue(Value& p_Out, size_t p_Depth)
            {
                if (!Count())
                    return false;

                SkipSpace();

                if (m_Pos >= m_Text.size())
                    return Error("unexpected end");

                const char c = m_Text[m_Pos];

                if (c == '{')
                    return ParseObject(p_Out, p_Depth + 1);
                if (c == '[')
                    return ParseArray(p_Out, p_Depth + 1);
                if (c == '"')
                {
                    p_Out.kind = Value::Kind::String;
                    return ParseString(p_Out.text);
                }
                if (c == 't' || c == 'f' || c == 'n')
                    return ParseLiteral(p_Out);
                if (c == '-' || (c >= '0' && c <= '9'))
                    return ParseNumber(p_Out);

                return Error("unexpected character");
            }

            bool ParseObject(Value& p_Out, size_t p_Depth)
            {
                if (p_Depth > k_MaxDepth)
                    return Error("too deep");

                p_Out.kind = Value::Kind::Object;
                ++m_Pos; // {

                if (Consume('}'))
                    return true;

                for (;;)
                {
                    SkipSpace();
                    if (m_Pos >= m_Text.size() || m_Text[m_Pos] != '"')
                        return Error("expected key");

                    std::string s_Key;
                    if (!ParseString(s_Key))
                        return false;

                    for (const auto& s_Field : p_Out.fields)
                        if (s_Field.first == s_Key)
                            return Error("duplicate key");

                    if (!Consume(':'))
                        return Error("expected colon");

                    Value s_Value;
                    if (!ParseValue(s_Value, p_Depth))
                        return false;

                    p_Out.fields.emplace_back(std::move(s_Key), std::move(s_Value));

                    if (Consume(','))
                        continue;
                    if (Consume('}'))
                        return true;
                    return Error("expected comma or brace");
                }
            }

            bool ParseArray(Value& p_Out, size_t p_Depth)
            {
                if (p_Depth > k_MaxDepth)
                    return Error("too deep");

                p_Out.kind = Value::Kind::Array;
                ++m_Pos; // [

                if (Consume(']'))
                    return true;

                for (;;)
                {
                    Value s_Item;
                    if (!ParseValue(s_Item, p_Depth))
                        return false;
                    p_Out.items.push_back(std::move(s_Item));

                    if (Consume(','))
                        continue;
                    if (Consume(']'))
                        return true;
                    return Error("expected comma or bracket");
                }
            }

            static bool Hex(char p_Char, uint32_t& p_Out)
            {
                if (p_Char >= '0' && p_Char <= '9') { p_Out = p_Char - '0'; return true; }
                if (p_Char >= 'a' && p_Char <= 'f') { p_Out = 10 + p_Char - 'a'; return true; }
                if (p_Char >= 'A' && p_Char <= 'F') { p_Out = 10 + p_Char - 'A'; return true; }
                return false;
            }

            bool ReadHex4(uint32_t& p_Out)
            {
                if (m_Pos + 4 > m_Text.size())
                    return Error("bad unicode escape");

                p_Out = 0;
                for (int k = 0; k < 4; ++k)
                {
                    uint32_t s_Digit;
                    if (!Hex(m_Text[m_Pos + k], s_Digit))
                        return Error("bad unicode escape");
                    p_Out = (p_Out << 4) | s_Digit;
                }

                m_Pos += 4;
                return true;
            }

            static void AppendUtf8(std::string& p_Out, uint32_t p_Code)
            {
                if (p_Code < 0x80) p_Out.push_back(static_cast<char>(p_Code));
                else if (p_Code < 0x800)
                {
                    p_Out.push_back(static_cast<char>(0xC0 | (p_Code >> 6)));
                    p_Out.push_back(static_cast<char>(0x80 | (p_Code & 0x3F)));
                }
                else if (p_Code < 0x10000)
                {
                    p_Out.push_back(static_cast<char>(0xE0 | (p_Code >> 12)));
                    p_Out.push_back(static_cast<char>(0x80 | ((p_Code >> 6) & 0x3F)));
                    p_Out.push_back(static_cast<char>(0x80 | (p_Code & 0x3F)));
                }
                else
                {
                    p_Out.push_back(static_cast<char>(0xF0 | (p_Code >> 18)));
                    p_Out.push_back(static_cast<char>(0x80 | ((p_Code >> 12) & 0x3F)));
                    p_Out.push_back(static_cast<char>(0x80 | ((p_Code >> 6) & 0x3F)));
                    p_Out.push_back(static_cast<char>(0x80 | (p_Code & 0x3F)));
                }
            }

            bool ParseString(std::string& p_Out)
            {
                ++m_Pos; // opening quote

                for (;;)
                {
                    if (m_Pos >= m_Text.size())
                        return Error("unterminated string");

                    const unsigned char c = static_cast<unsigned char>(m_Text[m_Pos++]);

                    if (c == '"')
                        return true;

                    if (c < 0x20)
                        return Error("control character in string");

                    if (c != '\\')
                    {
                        p_Out.push_back(static_cast<char>(c));
                    }
                    else
                    {
                        if (m_Pos >= m_Text.size())
                            return Error("bad escape");

                        const char e = m_Text[m_Pos++];
                        switch (e)
                        {
                            case '"': p_Out.push_back('"'); break;
                            case '\\': p_Out.push_back('\\'); break;
                            case '/': p_Out.push_back('/'); break;
                            case 'b': p_Out.push_back('\b'); break;
                            case 'f': p_Out.push_back('\f'); break;
                            case 'n': p_Out.push_back('\n'); break;
                            case 'r': p_Out.push_back('\r'); break;
                            case 't': p_Out.push_back('\t'); break;
                            case 'u':
                            {
                                uint32_t s_Code;
                                if (!ReadHex4(s_Code))
                                    return false;

                                if (s_Code >= 0xD800 && s_Code <= 0xDBFF)
                                {
                                    if (m_Pos + 2 > m_Text.size() || m_Text[m_Pos] != '\\' || m_Text[m_Pos + 1] != 'u')
                                        return Error("lone surrogate");
                                    m_Pos += 2;
                                    uint32_t s_Low;
                                    if (!ReadHex4(s_Low))
                                        return false;
                                    if (s_Low < 0xDC00 || s_Low > 0xDFFF)
                                        return Error("bad surrogate pair");
                                    s_Code = 0x10000 + ((s_Code - 0xD800) << 10) + (s_Low - 0xDC00);
                                }
                                else if (s_Code >= 0xDC00 && s_Code <= 0xDFFF)
                                {
                                    return Error("lone surrogate");
                                }

                                AppendUtf8(p_Out, s_Code);
                                break;
                            }
                            default:
                                return Error("bad escape");
                        }
                    }

                    if (p_Out.size() > k_MaxStringBytes)
                        return Error("string too long");
                }
            }

            bool ParseLiteral(Value& p_Out)
            {
                const std::string_view s_Rest = m_Text.substr(m_Pos);

                if (s_Rest.substr(0, 4) == "true") { p_Out.kind = Value::Kind::Bool; p_Out.boolean = true; m_Pos += 4; return true; }
                if (s_Rest.substr(0, 5) == "false") { p_Out.kind = Value::Kind::Bool; p_Out.boolean = false; m_Pos += 5; return true; }
                if (s_Rest.substr(0, 4) == "null") { p_Out.kind = Value::Kind::Null; m_Pos += 4; return true; }

                return Error("bad literal");
            }

            bool ParseNumber(Value& p_Out)
            {
                const size_t s_Start = m_Pos;
                bool s_Integer = true;

                if (m_Text[m_Pos] == '-')
                    ++m_Pos;

                if (m_Pos >= m_Text.size() || m_Text[m_Pos] < '0' || m_Text[m_Pos] > '9')
                    return Error("bad number");

                if (m_Text[m_Pos] == '0')
                {
                    ++m_Pos;
                    if (m_Pos < m_Text.size() && m_Text[m_Pos] >= '0' && m_Text[m_Pos] <= '9')
                        return Error("leading zero");
                }
                else
                {
                    while (m_Pos < m_Text.size() && m_Text[m_Pos] >= '0' && m_Text[m_Pos] <= '9')
                        ++m_Pos;
                }

                if (m_Pos < m_Text.size() && m_Text[m_Pos] == '.')
                {
                    s_Integer = false;
                    ++m_Pos;
                    if (m_Pos >= m_Text.size() || m_Text[m_Pos] < '0' || m_Text[m_Pos] > '9')
                        return Error("bad fraction");
                    while (m_Pos < m_Text.size() && m_Text[m_Pos] >= '0' && m_Text[m_Pos] <= '9')
                        ++m_Pos;
                }

                if (m_Pos < m_Text.size() && (m_Text[m_Pos] == 'e' || m_Text[m_Pos] == 'E'))
                {
                    s_Integer = false;
                    ++m_Pos;
                    if (m_Pos < m_Text.size() && (m_Text[m_Pos] == '+' || m_Text[m_Pos] == '-'))
                        ++m_Pos;
                    if (m_Pos >= m_Text.size() || m_Text[m_Pos] < '0' || m_Text[m_Pos] > '9')
                        return Error("bad exponent");
                    while (m_Pos < m_Text.size() && m_Text[m_Pos] >= '0' && m_Text[m_Pos] <= '9')
                        ++m_Pos;
                }

                const std::string s_Digits(m_Text.substr(s_Start, m_Pos - s_Start));

                if (s_Digits.size() > 32)
                    return Error("number too long");

                p_Out.kind = Value::Kind::Number;
                p_Out.number = std::strtod(s_Digits.c_str(), nullptr);

                if (s_Integer)
                {
                    char* s_End = nullptr;
                    const long long s_Value = std::strtoll(s_Digits.c_str(), &s_End, 10);
                    if (s_End && *s_End == '\0' && s_Digits.size() <= 19)
                    {
                        p_Out.is_integer = true;
                        p_Out.integer = s_Value;
                    }
                }

                return true;
            }

        private:
            std::string_view m_Text;
            size_t m_Pos = 0;
            size_t m_Nodes = 0;
            std::string m_Error;
        };
    }

    ParseResult Parse(std::string_view p_Text)
    {
        return Reader(p_Text).Run();
    }
}
