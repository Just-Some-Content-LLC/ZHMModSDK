#pragma once

// The three outfit definition ids of the B0 corpus (Tests/Fixtures/B0Disguise.h) in the form the
// engine actually passes them at runtime (M2 design, section 34): a 16-byte ZRepositoryID, given
// here as the little-endian memory image of its GUID fields (data1, data2, data3 least significant
// byte first; data4 in order). Each image was derived by hand from the dashed string the engine's
// own JSON writer printed in B0; the string beside it is that B0 text and is the expected rendering.
// Together with B0Disguise.h this lets a test build an observation whose Value went through the
// relay's own byte-to-text conversion instead of being lifted from the JSON corpus.

#include <array>
#include <cstddef>
#include <cstdint>

namespace B0Fixtures
{
    struct RepositoryIdImage
    {
        const char* dashed;                 // the B0 corpus text (engine JSON writer)
        std::array<uint8_t, 16> le_bytes;   // the GUID's little-endian memory image
    };

    // 874c4c48-0a8b-49e9-883e-49fc5f1fb051: the starting suit (StartingSuit, both sessions; ContractStart.Disguise).
    inline constexpr RepositoryIdImage k_SuitImage = {
        "874c4c48-0a8b-49e9-883e-49fc5f1fb051",
        {0x48, 0x4c, 0x4c, 0x87, 0x8b, 0x0a, 0xe9, 0x49, 0x88, 0x3e, 0x49, 0xfc, 0x5f, 0x1f, 0xb0, 0x51},
    };

    // 2018db77-aa8a-4bf9-9afb-56bdaa161156: outfit A (first change, first compromise, first clear).
    inline constexpr RepositoryIdImage k_OutfitAImage = {
        "2018db77-aa8a-4bf9-9afb-56bdaa161156",
        {0x77, 0xdb, 0x18, 0x20, 0x8a, 0xaa, 0xf9, 0x4b, 0x9a, 0xfb, 0x56, 0xbd, 0xaa, 0x16, 0x11, 0x56},
    };

    // 992cc7b6-4ccf-4ae8-a467-e9b2aabaeeb5: outfit B (second change, second compromise, second clear).
    inline constexpr RepositoryIdImage k_OutfitBImage = {
        "992cc7b6-4ccf-4ae8-a467-e9b2aabaeeb5",
        {0xb6, 0xc7, 0x2c, 0x99, 0xcf, 0x4c, 0xe8, 0x4a, 0xa4, 0x67, 0xe9, 0xb2, 0xaa, 0xba, 0xee, 0xb5},
    };

    // The image each of the 8 recorded disguise events carries, by k_Disguise index.
    inline const RepositoryIdImage* const k_DisguiseImages[] = {
        &k_SuitImage,    // k_StartingSuitA
        &k_OutfitAImage, // k_DisguiseChange1
        &k_OutfitAImage, // k_DisguiseBlown1
        &k_OutfitAImage, // k_DisguiseCleared1
        &k_OutfitBImage, // k_DisguiseChange2
        &k_OutfitBImage, // k_DisguiseBlown2
        &k_OutfitBImage, // k_DisguiseCleared2
        &k_SuitImage,    // k_StartingSuitB
    };

    inline constexpr size_t k_DisguiseImageCount = sizeof(k_DisguiseImages) / sizeof(k_DisguiseImages[0]);
}
