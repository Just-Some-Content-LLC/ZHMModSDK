#include "Json.h"

#include <fmt/format.h>

std::string Json::Quote(std::string_view p_Value)
{
    std::string s_Out;
    s_Out.reserve(p_Value.size() + 2);
    s_Out.push_back('"');

    for (const unsigned char s_Char : p_Value)
    {
        switch (s_Char)
        {
            case '"': s_Out += "\\\""; break;
            case '\\': s_Out += "\\\\"; break;
            case '\b': s_Out += "\\b"; break;
            case '\f': s_Out += "\\f"; break;
            case '\n': s_Out += "\\n"; break;
            case '\r': s_Out += "\\r"; break;
            case '\t': s_Out += "\\t"; break;
            default:
                if (s_Char < 0x20)
                    s_Out += fmt::format("\\u{:04x}", s_Char);
                else
                    s_Out.push_back(static_cast<char>(s_Char));
        }
    }

    s_Out.push_back('"');
    return s_Out;
}
