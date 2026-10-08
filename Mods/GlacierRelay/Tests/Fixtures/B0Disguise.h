#pragma once

// The 2 StartingSuit, 2 Disguise, 2 DisguiseBlown and 2 BrokenDisguiseCleared events the game emitted in the
// B0 probe (2026-10-07, Paris, game 3.280.0.0), verbatim from the native log except that the user and platform
// session identifiers are removed. Every Value is a bare string holding an outfit definition repository id
// (glacier-relay M2 design, section 30.1). Generated from the B0 corpus; do not edit by hand.

#include <cstddef>

namespace B0Fixtures
{
    struct RawDisguiseEvent { int probe_sequence; int event_index; int frame; const char* json; };

    inline const RawDisguiseEvent k_Disguise[] = {
        {9, 10, 40230, R"json({"Timestamp":13.011781,"Name":"StartingSuit","ContractSessionId":"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f","ContractId":"00000000-0000-0000-0000-000000000200","Value":"874c4c48-0a8b-49e9-883e-49fc5f1fb051","Origin":"gameclient","Id":"3b200548-34cc-42d8-a92a-e51a7ceaa93a"})json"},
        {16, 17, 60281, R"json({"Timestamp":202.104156,"Name":"Disguise","ContractSessionId":"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f","ContractId":"00000000-0000-0000-0000-000000000200","Value":"2018db77-aa8a-4bf9-9afb-56bdaa161156","Origin":"gameclient","Id":"3d83fef5-bcee-4ec6-9b5d-0557f3735fdb"})json"},
        {33, 34, 62901, R"json({"Timestamp":222.863129,"Name":"DisguiseBlown","ContractSessionId":"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f","ContractId":"00000000-0000-0000-0000-000000000200","Value":"2018db77-aa8a-4bf9-9afb-56bdaa161156","Origin":"gameclient","Id":"6209107e-f324-4cf3-a7d9-c085d9815272"})json"},
        {57, 58, 84058, R"json({"Timestamp":393.788727,"Name":"BrokenDisguiseCleared","ContractSessionId":"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f","ContractId":"00000000-0000-0000-0000-000000000200","Value":"2018db77-aa8a-4bf9-9afb-56bdaa161156","Origin":"gameclient","Id":"28a079c9-2f79-4b53-8dc0-902239089601"})json"},
        {73, 74, 96764, R"json({"Timestamp":497.756409,"Name":"Disguise","ContractSessionId":"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f","ContractId":"00000000-0000-0000-0000-000000000200","Value":"992cc7b6-4ccf-4ae8-a467-e9b2aabaeeb5","Origin":"gameclient","Id":"11798f2f-32a0-4ea9-8c62-600c40c21c4d"})json"},
        {86, 88, 112230, R"json({"Timestamp":615.012756,"Name":"DisguiseBlown","ContractSessionId":"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f","ContractId":"00000000-0000-0000-0000-000000000200","Value":"992cc7b6-4ccf-4ae8-a467-e9b2aabaeeb5","Origin":"gameclient","Id":"9c0827b6-af67-450e-957f-68d56fc1f5c4"})json"},
        {102, 105, 113243, R"json({"Timestamp":624.086853,"Name":"BrokenDisguiseCleared","ContractSessionId":"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f","ContractId":"00000000-0000-0000-0000-000000000200","Value":"992cc7b6-4ccf-4ae8-a467-e9b2aabaeeb5","Origin":"gameclient","Id":"b9bdf609-968d-4a0b-9aef-e485c05deb03"})json"},
        {198, 205, 146722, R"json({"Timestamp":2.280066,"Name":"StartingSuit","ContractSessionId":"2516109618691980006-9666b5ad-6a4f-44bb-b5fb-ff86bb3a8d76","ContractId":"00000000-0000-0000-0000-000000000200","Value":"874c4c48-0a8b-49e9-883e-49fc5f1fb051","Origin":"gameclient","Id":"8b7ccad6-a372-4304-939e-9d0c4683a874"})json"},
    };

    inline constexpr size_t k_DisguiseCount = sizeof(k_Disguise) / sizeof(k_Disguise[0]);

    // Indices into k_Disguise, by role in the B0 session (order of emission).
    inline constexpr size_t k_StartingSuitA   = 0; // fresh entry, session A, same frame as IntroCutEnd (Timestamp 13.012)
    inline constexpr size_t k_DisguiseChange1 = 1; // first change: 2018db77 (Timestamp 202.104)
    inline constexpr size_t k_DisguiseBlown1  = 2; // compromise of 2018db77 (same Timestamp as a Spotted)
    inline constexpr size_t k_DisguiseCleared1 = 3; // clear of 2018db77, 9 ms after a Kill
    inline constexpr size_t k_DisguiseChange2 = 4; // second change: 992cc7b6
    inline constexpr size_t k_DisguiseBlown2  = 5; // compromise of 992cc7b6
    inline constexpr size_t k_DisguiseCleared2 = 6; // clear of 992cc7b6, 11 ms after a Kill
    inline constexpr size_t k_StartingSuitB   = 7; // after the restart, session B (Timestamp 2.280)
}
