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
