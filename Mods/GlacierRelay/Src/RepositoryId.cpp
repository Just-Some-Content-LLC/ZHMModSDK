#include "RepositoryId.h"

namespace
{
    constexpr char k_HexDigits[] = "0123456789abcdef";

    void AppendByte(std::string& p_Out, uint8_t p_Byte)
    {
        p_Out += k_HexDigits[p_Byte >> 4];
        p_Out += k_HexDigits[p_Byte & 0x0F];
    }

    // Most significant byte first, whatever the host's byte order.
    void AppendUInt32(std::string& p_Out, uint32_t p_Value)
    {
        AppendByte(p_Out, static_cast<uint8_t>(p_Value >> 24));
        AppendByte(p_Out, static_cast<uint8_t>(p_Value >> 16));
        AppendByte(p_Out, static_cast<uint8_t>(p_Value >> 8));
        AppendByte(p_Out, static_cast<uint8_t>(p_Value));
    }

    void AppendUInt16(std::string& p_Out, uint16_t p_Value)
    {
        AppendByte(p_Out, static_cast<uint8_t>(p_Value >> 8));
        AppendByte(p_Out, static_cast<uint8_t>(p_Value));
    }
}

RepositoryId RepositoryId::FromLittleEndianBytes(const std::array<uint8_t, k_Bytes>& p_Bytes)
{
    RepositoryId s_Id;

    s_Id.data1 = static_cast<uint32_t>(p_Bytes[0])
        | (static_cast<uint32_t>(p_Bytes[1]) << 8)
        | (static_cast<uint32_t>(p_Bytes[2]) << 16)
        | (static_cast<uint32_t>(p_Bytes[3]) << 24);
    s_Id.data2 = static_cast<uint16_t>(p_Bytes[4] | (p_Bytes[5] << 8));
    s_Id.data3 = static_cast<uint16_t>(p_Bytes[6] | (p_Bytes[7] << 8));

    for (size_t i = 0; i < s_Id.data4.size(); ++i)
        s_Id.data4[i] = p_Bytes[8 + i];

    return s_Id;
}

namespace
{
    bool HexValue(char p_Char, uint8_t& p_Out)
    {
        if (p_Char >= '0' && p_Char <= '9')
        {
            p_Out = static_cast<uint8_t>(p_Char - '0');
            return true;
        }
        if (p_Char >= 'a' && p_Char <= 'f')
        {
            p_Out = static_cast<uint8_t>(10 + p_Char - 'a');
            return true;
        }
        return false;
    }

    bool ReadByte(std::string_view p_Text, size_t p_At, uint8_t& p_Out)
    {
        uint8_t s_High = 0, s_Low = 0;
        if (!HexValue(p_Text[p_At], s_High) || !HexValue(p_Text[p_At + 1], s_Low))
            return false;
        p_Out = static_cast<uint8_t>((s_High << 4) | s_Low);
        return true;
    }
}

bool RepositoryId::FromDashedLowercase(std::string_view p_Text, RepositoryId& p_Out)
{
    if (p_Text.size() != k_TextLength || p_Text[8] != '-' || p_Text[13] != '-' || p_Text[18] != '-' || p_Text[23] != '-')
        return false;

    // Most significant digit first for the three integer fields, bytes in order for data4.
    uint8_t s_Bytes[16];
    const size_t s_Starts[16] = {0, 2, 4, 6, 9, 11, 14, 16, 19, 21, 24, 26, 28, 30, 32, 34};

    for (size_t i = 0; i < 16; ++i)
        if (!ReadByte(p_Text, s_Starts[i], s_Bytes[i]))
            return false;

    RepositoryId s_Id;
    s_Id.data1 = (static_cast<uint32_t>(s_Bytes[0]) << 24) | (static_cast<uint32_t>(s_Bytes[1]) << 16) | (static_cast<uint32_t>(s_Bytes[2]) << 8) | s_Bytes[3];
    s_Id.data2 = static_cast<uint16_t>((s_Bytes[4] << 8) | s_Bytes[5]);
    s_Id.data3 = static_cast<uint16_t>((s_Bytes[6] << 8) | s_Bytes[7]);
    for (size_t i = 0; i < 8; ++i)
        s_Id.data4[i] = s_Bytes[8 + i];

    p_Out = s_Id;
    return true;
}

std::array<uint8_t, RepositoryId::k_Bytes> RepositoryId::ToLittleEndianBytes() const
{
    std::array<uint8_t, k_Bytes> s_Bytes{};
    s_Bytes[0] = static_cast<uint8_t>(data1);
    s_Bytes[1] = static_cast<uint8_t>(data1 >> 8);
    s_Bytes[2] = static_cast<uint8_t>(data1 >> 16);
    s_Bytes[3] = static_cast<uint8_t>(data1 >> 24);
    s_Bytes[4] = static_cast<uint8_t>(data2);
    s_Bytes[5] = static_cast<uint8_t>(data2 >> 8);
    s_Bytes[6] = static_cast<uint8_t>(data3);
    s_Bytes[7] = static_cast<uint8_t>(data3 >> 8);
    for (size_t i = 0; i < data4.size(); ++i)
        s_Bytes[8 + i] = data4[i];
    return s_Bytes;
}

std::string RepositoryId::ToDashedLowercase() const
{
    std::string s_Out;
    s_Out.reserve(k_TextLength);

    AppendUInt32(s_Out, data1);
    s_Out += '-';
    AppendUInt16(s_Out, data2);
    s_Out += '-';
    AppendUInt16(s_Out, data3);
    s_Out += '-';
    AppendByte(s_Out, data4[0]);
    AppendByte(s_Out, data4[1]);
    s_Out += '-';

    for (size_t i = 2; i < data4.size(); ++i)
        AppendByte(s_Out, data4[i]);

    return s_Out;
}
