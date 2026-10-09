#pragma once

// The seven item definition ids of the B0 corpus (Tests/Fixtures/B0Items.h) in the form the engine passes a
// ZRepositoryID at runtime (M2 design, section 34): the 16-byte little-endian memory image of its GUID fields
// (data1, data2, data3 least significant byte first; data4 in order). Each image was derived from the dashed
// string the engine's own JSON writer printed in B0; the string beside it is that B0 text and is the expected
// rendering. Whether the item object's RepositoryId is a ZRepositoryID or a ZString at runtime is NOT known
// (section 38.1 C): these images let a test exercise the production renderer on the item ids either way.
// Generated from the B0 corpus; do not edit by hand.

#include <array>
#include <cstddef>
#include <cstdint>

#include "B0DisguiseBytes.h"

namespace B0Fixtures
{
    // 6adddf7e-6879-4d51-a7e2-6a25ffdca6ae: Wrench
    inline constexpr RepositoryIdImage k_ItemImage1 = {
        "6adddf7e-6879-4d51-a7e2-6a25ffdca6ae",
        {0x7e, 0xdf, 0xdd, 0x6a, 0x79, 0x68, 0x51, 0x4d, 0xa7, 0xe2, 0x6a, 0x25, 0xff, 0xdc, 0xa6, 0xae},
    };

    // 01ed6d15-e26e-4362-b1a6-363684a7d0fd: Crowbar
    inline constexpr RepositoryIdImage k_ItemImage2 = {
        "01ed6d15-e26e-4362-b1a6-363684a7d0fd",
        {0x15, 0x6d, 0xed, 0x01, 0x6e, 0xe2, 0x62, 0x43, 0xb1, 0xa6, 0x36, 0x36, 0x84, 0xa7, 0xd0, 0xfd},
    };

    // 8b37a3a8-8a20-4262-81c5-0fcd15f4bba9: Emetic Rat Poison
    inline constexpr RepositoryIdImage k_ItemImage3 = {
        "8b37a3a8-8a20-4262-81c5-0fcd15f4bba9",
        {0xa8, 0xa3, 0x37, 0x8b, 0x20, 0x8a, 0x62, 0x42, 0x81, 0xc5, 0x0f, 0xcd, 0x15, 0xf4, 0xbb, 0xa9},
    };

    // 7aeb740f-3d60-4e49-8d27-15a98067ce9f: Lead Pipe
    inline constexpr RepositoryIdImage k_ItemImage4 = {
        "7aeb740f-3d60-4e49-8d27-15a98067ce9f",
        {0x0f, 0x74, 0xeb, 0x7a, 0x60, 0x3d, 0x49, 0x4e, 0x8d, 0x27, 0x15, 0xa9, 0x80, 0x67, 0xce, 0x9f},
    };

    // e17172cc-bf70-4df6-9828-d9856b1a24fd: Kitchen Knife
    inline constexpr RepositoryIdImage k_ItemImage5 = {
        "e17172cc-bf70-4df6-9828-d9856b1a24fd",
        {0xcc, 0x72, 0x71, 0xe1, 0x70, 0xbf, 0xf6, 0x4d, 0x98, 0x28, 0xd9, 0x85, 0x6b, 0x1a, 0x24, 0xfd},
    };

    // 1bbf0ed5-0515-4599-a4c9-454ce59cff44: Cleaver
    inline constexpr RepositoryIdImage k_ItemImage6 = {
        "1bbf0ed5-0515-4599-a4c9-454ce59cff44",
        {0xd5, 0x0e, 0xbf, 0x1b, 0x15, 0x05, 0x99, 0x45, 0xa4, 0xc9, 0x45, 0x4c, 0xe5, 0x9c, 0xff, 0x44},
    };

    // a8a0c154-c36f-413e-8f29-b83a1b7a22f0: Propane Flask
    inline constexpr RepositoryIdImage k_ItemImage7 = {
        "a8a0c154-c36f-413e-8f29-b83a1b7a22f0",
        {0x54, 0xc1, 0xa0, 0xa8, 0x6f, 0xc3, 0x3e, 0x41, 0x8f, 0x29, 0xb8, 0x3a, 0x1b, 0x7a, 0x22, 0xf0},
    };

    // The seven definition images in first-seen order (the k_ItemDefinition<n> order of B0Items.h).
    inline constexpr const RepositoryIdImage* k_ItemDefinitionImages[] = {
        &k_ItemImage1, &k_ItemImage2, &k_ItemImage3, &k_ItemImage4, &k_ItemImage5, &k_ItemImage6, &k_ItemImage7
    };

    // The image of each event's RepositoryId, by k_Items index.
    inline constexpr const RepositoryIdImage* k_ItemImages[] = {
        &k_ItemImage1, // ItemPickedUp Wrench
        &k_ItemImage1, // ItemRemovedFromInventory Wrench
        &k_ItemImage1, // ItemThrown Wrench
        &k_ItemImage1, // ItemPickedUp Wrench
        &k_ItemImage1, // ItemRemovedFromInventory Wrench
        &k_ItemImage1, // ItemThrown Wrench
        &k_ItemImage1, // ItemPickedUp Wrench
        &k_ItemImage2, // ItemPickedUp Crowbar
        &k_ItemImage2, // ItemRemovedFromInventory Crowbar
        &k_ItemImage2, // ItemThrown Crowbar
        &k_ItemImage2, // ItemPickedUp Crowbar
        &k_ItemImage2, // ItemPickedUp Crowbar
        &k_ItemImage3, // ItemPickedUp Emetic Rat Poison
        &k_ItemImage4, // ItemPickedUp Lead Pipe
        &k_ItemImage5, // ItemPickedUp Kitchen Knife
        &k_ItemImage6, // ItemPickedUp Cleaver
        &k_ItemImage4, // ItemRemovedFromInventory Lead Pipe
        &k_ItemImage4, // ItemThrown Lead Pipe
        &k_ItemImage5, // ItemRemovedFromInventory Kitchen Knife
        &k_ItemImage5, // ItemThrown Kitchen Knife
        &k_ItemImage5, // ItemPickedUp Kitchen Knife
        &k_ItemImage7, // ItemPickedUp Propane Flask
        &k_ItemImage7, // ItemRemovedFromInventory Propane Flask
        &k_ItemImage7, // ItemThrown Propane Flask
    };
}
