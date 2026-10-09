#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

// A repository id as the relay owns it: the four fields of a Microsoft-layout GUID, which is what
// the engine's ZRepositoryID (a ZGuid) holds. The Glacier-facing intake copies the fields out of the
// engine value; everything here is engine-independent and is the only renderer of such ids.
//
// Field semantics are preserved on purpose: data1, data2 and data3 are integers and print most
// significant digit first; data4 is eight bytes printed in order. The dashed text is therefore NOT a
// hex dump of the 16-byte memory image (on the little-endian engine the first three fields are
// stored least significant byte first). M2 design, section 35.
struct RepositoryId
{
    static constexpr size_t k_Bytes = 16;
    static constexpr size_t k_TextLength = 36;

    uint32_t data1 = 0;
    uint16_t data2 = 0;
    uint16_t data3 = 0;
    std::array<uint8_t, 8> data4{};

    // Interprets p_Bytes as the GUID's little-endian memory image: data1 at bytes 0..3, data2 at
    // 4..5, data3 at 6..7 (each least significant byte first), data4 at 8..15 in order. This is the
    // layout the SDK declares for ZGuid on x64 (TelemetryIntake.cpp pins it at compile time).
    static RepositoryId FromLittleEndianBytes(const std::array<uint8_t, k_Bytes>& p_Bytes);

    // The dashed lowercase form, exactly k_TextLength characters:
    // xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx (data1-data2-data3-data4[0..1]-data4[2..7]).
    std::string ToDashedLowercase() const;

    bool operator==(const RepositoryId&) const = default;
};
