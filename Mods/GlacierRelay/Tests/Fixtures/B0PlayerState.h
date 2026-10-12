#pragma once

// The 2 Trespassing and 4 HoldingIllegalWeapon events the game emitted in the B0 probe (2026-10-07, Paris, game
// 3.280.0.0), verbatim from the native log except that the user and platform session identifiers are removed.
// Trespassing: {IsTrespassing: bool, RoomId: number} (one true, one false). HoldingIllegalWeapon: {IsHoldingIllegalWeapon:
// true, WeaponEquipped: {IsPerceivedAsWeapon, InstanceId (non-empty), ItemType, ItemName, RepositoryId, OnlineTraits,
// Category, ActionRewardType}} on the two true forms; {IsHoldingIllegalWeapon: false} with no WeaponEquipped on the two
// false forms (glacier-relay M2 design, section 46.1). The engine field types are unknown: the JSON writer's text is
// the only evidence. Generated from the B0 corpus; do not edit by hand.

#include <cstddef>

namespace B0Fixtures
{
    struct RawPlayerStateEvent { int probe_sequence; int event_index; int frame; const char* json; };

    inline const RawPlayerStateEvent k_PlayerState[] = {
        {14, 15, 56037, R"json({"Timestamp":166.198425,"Name":"Trespassing","ContractSessionId":"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f","ContractId":"00000000-0000-0000-0000-000000000200","Value":{"IsTrespassing":true,"RoomId":6.000000},"Origin":"gameclient","Id":"a7f8f538-704e-4684-85f6-a468ac9937de"})json"},
        {18, 19, 60307, R"json({"Timestamp":202.179184,"Name":"Trespassing","ContractSessionId":"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f","ContractId":"00000000-0000-0000-0000-000000000200","Value":{"IsTrespassing":false,"RoomId":3.000000},"Origin":"gameclient","Id":"2d071288-2c3a-43e2-9b7d-1d287d89468f"})json"},
        {64, 65, 94325, R"json({"Timestamp":476.573425,"Name":"HoldingIllegalWeapon","ContractSessionId":"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f","ContractId":"00000000-0000-0000-0000-000000000200","Value":{"IsHoldingIllegalWeapon":true,"WeaponEquipped":{"IsPerceivedAsWeapon":true,"InstanceId":"9a3f1dbb-a8b9-407a-8a7f-cdce8ddf1955","ItemType":"Gun_HardBaller_01","ItemName":"ICA19 Silverballer","RepositoryId":"e70adb5b-0646-4f88-bd4a-85bea7a2a654","OnlineTraits":["pistol"],"Category":null,"ActionRewardType":"AR_None"}},"Origin":"gameclient","Id":"920e89f1-1be6-4b7a-bfc2-464ff4b95ee8"})json"},
        {65, 66, 94445, R"json({"Timestamp":477.591370,"Name":"HoldingIllegalWeapon","ContractSessionId":"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f","ContractId":"00000000-0000-0000-0000-000000000200","Value":{"IsHoldingIllegalWeapon":false},"Origin":"gameclient","Id":"2d914b22-8e97-4c13-805e-35b7bcef1aac"})json"},
        {66, 67, 95360, R"json({"Timestamp":485.492706,"Name":"HoldingIllegalWeapon","ContractSessionId":"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f","ContractId":"00000000-0000-0000-0000-000000000200","Value":{"IsHoldingIllegalWeapon":true,"WeaponEquipped":{"IsPerceivedAsWeapon":true,"InstanceId":"9a3f1dbb-a8b9-407a-8a7f-cdce8ddf1955","ItemType":"Gun_HardBaller_01","ItemName":"ICA19 Silverballer","RepositoryId":"e70adb5b-0646-4f88-bd4a-85bea7a2a654","OnlineTraits":["pistol"],"Category":null,"ActionRewardType":"AR_None"}},"Origin":"gameclient","Id":"6f64c95b-5040-4403-8513-7aa8a621968d"})json"},
        {72, 73, 96110, R"json({"Timestamp":491.880981,"Name":"HoldingIllegalWeapon","ContractSessionId":"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f","ContractId":"00000000-0000-0000-0000-000000000200","Value":{"IsHoldingIllegalWeapon":false},"Origin":"gameclient","Id":"549a1486-8f3a-4f86-8913-f91eb721091e"})json"},
    };

    inline constexpr size_t k_PlayerStateCount = sizeof(k_PlayerState) / sizeof(k_PlayerState[0]);

    // Indices into k_PlayerState, by role in the B0 session (order of emission).
    inline constexpr size_t k_TrespassingEntered       = 0; // Trespassing index 15, Timestamp 166.198425: IsTrespassing true, RoomId 6
    inline constexpr size_t k_TrespassingLeft          = 1; // Trespassing index 19, Timestamp 202.179184: IsTrespassing false, RoomId 3; 25 ms after the first Disguise
    inline constexpr size_t k_IllegalWeaponDrawn1      = 2; // HoldingIllegalWeapon index 65, Timestamp 476.573425: IsHoldingIllegalWeapon true, WeaponEquipped (the loadout pistol)
    inline constexpr size_t k_IllegalWeaponHolstered1  = 3; // HoldingIllegalWeapon index 66, Timestamp 477.59137: IsHoldingIllegalWeapon false, no WeaponEquipped
    inline constexpr size_t k_IllegalWeaponDrawn2      = 4; // HoldingIllegalWeapon index 67, Timestamp 485.492706: IsHoldingIllegalWeapon true, WeaponEquipped (the loadout pistol)
    inline constexpr size_t k_IllegalWeaponHolstered2  = 5; // HoldingIllegalWeapon index 73, Timestamp 491.880981: IsHoldingIllegalWeapon false, no WeaponEquipped

    // The one weapon the two true forms named: a definition id and, unlike every B0 world item, a non-empty instance id.
    inline constexpr const char* k_IllegalWeaponDefinition = "e70adb5b-0646-4f88-bd4a-85bea7a2a654"; // ICA19 Silverballer
    inline constexpr const char* k_IllegalWeaponInstance = "9a3f1dbb-a8b9-407a-8a7f-cdce8ddf1955";
}
