#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// The canonical float-word encoding of M6-F (glacier-relay docs/design/M6D2_NEXT_STEP_DESIGN.md
// section 12.3): one stored IEEE-754 binary32 value travels as its exact bit pattern, eight
// lowercase hex digits, most significant nibble first; a group is its words joined by "," with no
// spaces. The conversion is a bit copy in both directions, never through rounded text, so two
// states that share a seven-significant-digit rendering and differ in their bits stay different.
// Validation is exact: the word count, exactly eight hex digits per word, every word finite (an
// exponent field of 0xff — NaN and the infinities — is refused); a signed zero is a valid, distinct
// word (00000000 and 80000000). Nothing here is an engine type, a pointer or an address; this file
// depends on fmt only and is shared by the protocol decoder, the observation encoders, the gate,
// the tests and the probe.
namespace FloatWords
{
    constexpr size_t k_WorldWords = 12; // SMatrix43 in its declared order: XAxis.xyz, YAxis.xyz, ZAxis.xyz, Trans.xyz
    constexpr size_t k_LocalWords = 7;  // m_vObjectToParentTranslation.xyz, m_vObjectToParentRotation.xyzw
    constexpr size_t k_WordChars = 8;
    constexpr size_t k_WorldTextBytes = k_WorldWords * k_WordChars + (k_WorldWords - 1); // 107
    constexpr size_t k_LocalTextBytes = k_LocalWords * k_WordChars + (k_LocalWords - 1);  // 62

    struct Words12
    {
        uint32_t w[k_WorldWords] = {};

        bool operator==(const Words12& p_Other) const
        {
            for (size_t i = 0; i < k_WorldWords; ++i)
                if (w[i] != p_Other.w[i])
                    return false;
            return true;
        }

        bool operator!=(const Words12& p_Other) const { return !(*this == p_Other); }
    };

    struct Words7
    {
        uint32_t w[k_LocalWords] = {};

        bool operator==(const Words7& p_Other) const
        {
            for (size_t i = 0; i < k_LocalWords; ++i)
                if (w[i] != p_Other.w[i])
                    return false;
            return true;
        }

        bool operator!=(const Words7& p_Other) const { return !(*this == p_Other); }
    };

    uint32_t ToWord(float p_Value);  // the bit pattern (memcpy)
    float ToFloat(uint32_t p_Word);  // the bit pattern back (memcpy)
    bool Finite(uint32_t p_Word);    // exponent field != 0xff
    bool AllFinite(const Words12& p_Words);
    bool AllFinite(const Words7& p_Words);

    std::string Hex8(uint32_t p_Word);
    std::string Encode(const uint32_t* p_Words, size_t p_Count); // "w,w,...,w"
    std::string Encode(const Words12& p_Words);
    std::string Encode(const Words7& p_Words);

    // Strict decoding: exactly p_Count words, each exactly eight lowercase hex digits, every one
    // finite; false with a bounded reason otherwise and p_Out untouched.
    bool Decode(std::string_view p_Text, uint32_t* p_Out, size_t p_Count, std::string& p_Why);
    bool Decode(std::string_view p_Text, Words12& p_Out, std::string& p_Why);
    bool Decode(std::string_view p_Text, Words7& p_Out, std::string& p_Why);

    // The human companions (seven significant digits, "%.7g" of the value; reading only, never a
    // decision): "x,y,z" of the translation words 9..11, and "tx,ty,tz|qx,qy,qz,qw".
    std::string Number7(float p_Value);
    std::string TranslationText(const Words12& p_Words);
    std::string LocalText(const Words7& p_Words);

    // The declared comparison policy's one tolerance (section 12.8): every word of both groups
    // finite and |a - b| <= p_Tolerance as floats. Agreement within it is never equality.
    bool WithinTolerance(const Words12& p_A, const Words12& p_B, float p_Tolerance);
}
