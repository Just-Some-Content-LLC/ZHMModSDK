#pragma once

// The ObjectiveCompleted event(s) the game emitted in the B0 probe (2026-10-07, Paris, game 3.280.0.0): one, 13 ms
// after the Kill of the target Viktor Novikov, verbatim from the native log except that the user and platform
// session identifiers are removed. The envelope is the "Name-first" variant (top-level XboxGameMode/XboxDifficulty,
// Timestamp after Value) that B0 also showed on ContractStart/ContractFailed/Spotted/Witnesses (glacier-relay M2
// design, section 42.1). Value: {Id, Type: "kill", Category: "primary", ExcludeFromScoring: false}; the engine field
// types are unknown. Generated from the B0 corpus; do not edit by hand.

#include <cstddef>

namespace B0Fixtures
{
    struct RawObjectiveEvent { int probe_sequence; int event_index; int frame; const char* json; };

    inline const RawObjectiveEvent k_Objectives[] = {
        {157, 163, 128572, R"json({"Name":"ObjectiveCompleted","ContractSessionId":"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f","ContractId":"00000000-0000-0000-0000-000000000200","Value":{"Id":"aca8cd5b-e3a3-4a60-b953-c590484f0491","Type":"kill","Category":"primary","ExcludeFromScoring":false},"XboxGameMode":3.000000,"XboxDifficulty":0.000000,"Timestamp":759.401611,"Origin":"gameclient","Id":"34d96c08-37db-4a87-afd4-793558011db0"})json"},
    };

    inline constexpr size_t k_ObjectiveCount = sizeof(k_Objectives) / sizeof(k_Objectives[0]);

    inline constexpr size_t k_ObjectiveNovikovKill = 0; // Timestamp 759.401611, after Kill index 162
    inline constexpr const char* k_ObjectiveNovikovId = "aca8cd5b-e3a3-4a60-b953-c590484f0491"; // opaque; compared with nothing
}
