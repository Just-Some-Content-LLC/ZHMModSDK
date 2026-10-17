#include "FloatWords.h"

#include <cmath>
#include <cstring>

#include <fmt/format.h>

namespace FloatWords
{
    uint32_t ToWord(float p_Value)
    {
        static_assert(sizeof(float) == sizeof(uint32_t), "binary32 expected");
        uint32_t s_Word = 0;
        std::memcpy(&s_Word, &p_Value, sizeof(s_Word));
        return s_Word;
    }

    float ToFloat(uint32_t p_Word)
    {
        float s_Value = 0.f;
        std::memcpy(&s_Value, &p_Word, sizeof(s_Value));
        return s_Value;
    }

    bool Finite(uint32_t p_Word)
    {
        return ((p_Word >> 23) & 0xffu) != 0xffu;
    }

    bool AllFinite(const Words12& p_Words)
    {
        for (const uint32_t s_Word : p_Words.w)
            if (!Finite(s_Word))
                return false;
        return true;
    }

    bool AllFinite(const Words7& p_Words)
    {
        for (const uint32_t s_Word : p_Words.w)
            if (!Finite(s_Word))
                return false;
        return true;
    }

    std::string Hex8(uint32_t p_Word)
    {
        return fmt::format("{:08x}", p_Word);
    }

    std::string Encode(const uint32_t* p_Words, size_t p_Count)
    {
        std::string s_Text;
        s_Text.reserve(p_Count * (k_WordChars + 1));
        for (size_t i = 0; i < p_Count; ++i)
        {
            if (i > 0)
                s_Text += ',';
            s_Text += Hex8(p_Words[i]);
        }
        return s_Text;
    }

    std::string Encode(const Words12& p_Words)
    {
        return Encode(p_Words.w, k_WorldWords);
    }

    std::string Encode(const Words7& p_Words)
    {
        return Encode(p_Words.w, k_LocalWords);
    }

    namespace
    {
        bool HexNibble(char p_Char, uint32_t& p_Out)
        {
            if (p_Char >= '0' && p_Char <= '9')
            {
                p_Out = static_cast<uint32_t>(p_Char - '0');
                return true;
            }
            if (p_Char >= 'a' && p_Char <= 'f')
            {
                p_Out = static_cast<uint32_t>(p_Char - 'a' + 10);
                return true;
            }
            return false; // uppercase is not the canonical form and is refused
        }
    }

    bool Decode(std::string_view p_Text, uint32_t* p_Out, size_t p_Count, std::string& p_Why)
    {
        const size_t s_Expected = p_Count * k_WordChars + (p_Count - 1);
        if (p_Text.size() != s_Expected)
        {
            p_Why = fmt::format("expected {} words ({} characters), got {} characters", p_Count, s_Expected, p_Text.size());
            return false;
        }

        uint32_t s_Words[k_WorldWords] = {};
        size_t s_Pos = 0;
        for (size_t i = 0; i < p_Count; ++i)
        {
            if (i > 0)
            {
                if (p_Text[s_Pos] != ',')
                {
                    p_Why = fmt::format("word {} not separated by a comma", i);
                    return false;
                }
                ++s_Pos;
            }

            uint32_t s_Word = 0;
            for (size_t j = 0; j < k_WordChars; ++j)
            {
                uint32_t s_Nibble = 0;
                if (!HexNibble(p_Text[s_Pos + j], s_Nibble))
                {
                    p_Why = fmt::format("word {} is not eight lowercase hex digits", i);
                    return false;
                }
                s_Word = (s_Word << 4) | s_Nibble;
            }
            s_Pos += k_WordChars;

            if (!Finite(s_Word))
            {
                p_Why = fmt::format("word {} is not finite", i);
                return false;
            }
            s_Words[i] = s_Word;
        }

        for (size_t i = 0; i < p_Count; ++i)
            p_Out[i] = s_Words[i];
        return true;
    }

    bool Decode(std::string_view p_Text, Words12& p_Out, std::string& p_Why)
    {
        Words12 s_Words;
        if (!Decode(p_Text, s_Words.w, k_WorldWords, p_Why))
            return false;
        p_Out = s_Words;
        return true;
    }

    bool Decode(std::string_view p_Text, Words7& p_Out, std::string& p_Why)
    {
        Words7 s_Words;
        if (!Decode(p_Text, s_Words.w, k_LocalWords, p_Why))
            return false;
        p_Out = s_Words;
        return true;
    }

    std::string Number7(float p_Value)
    {
        return fmt::format("{:.7g}", static_cast<double>(p_Value));
    }

    std::string TranslationText(const Words12& p_Words)
    {
        return Number7(ToFloat(p_Words.w[9])) + "," + Number7(ToFloat(p_Words.w[10])) + "," + Number7(ToFloat(p_Words.w[11]));
    }

    std::string LocalText(const Words7& p_Words)
    {
        return Number7(ToFloat(p_Words.w[0])) + "," + Number7(ToFloat(p_Words.w[1])) + "," + Number7(ToFloat(p_Words.w[2])) + "|"
            + Number7(ToFloat(p_Words.w[3])) + "," + Number7(ToFloat(p_Words.w[4])) + "," + Number7(ToFloat(p_Words.w[5])) + "," + Number7(ToFloat(p_Words.w[6]));
    }

    bool WithinTolerance(const Words12& p_A, const Words12& p_B, float p_Tolerance)
    {
        if (!AllFinite(p_A) || !AllFinite(p_B))
            return false;
        for (size_t i = 0; i < k_WorldWords; ++i)
        {
            const float s_Delta = std::fabs(ToFloat(p_A.w[i]) - ToFloat(p_B.w[i]));
            if (!(s_Delta <= p_Tolerance))
                return false;
        }
        return true;
    }
}
