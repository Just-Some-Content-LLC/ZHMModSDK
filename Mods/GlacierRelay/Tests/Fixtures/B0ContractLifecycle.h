#pragma once

// The 2 ContractStart and 2 ContractFailed events the game emitted in the B0 probe (2026-10-07, Paris,
// game 3.280.0.0), verbatim from the native log except that the user and platform session identifiers
// are removed. Their observed position relative to the mission predicate is recorded in the glacier-relay
// M2 design, section 27.3. Generated from the B0 corpus; do not edit by hand.

#include <cstddef>

namespace B0Fixtures
{
    struct RawContractEvent { int probe_sequence; int event_index; int frame; const char* json; };

    inline const RawContractEvent k_ContractLifecycle[] = {
        {2, 2, 39043, R"json({"Name":"ContractStart","ContractSessionId":"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f","ContractId":"00000000-0000-0000-0000-000000000200","Value":{"Loadout":[{"RepositoryId":"e70adb5b-0646-4f88-bd4a-85bea7a2a654","InstanceId":"9a3f1dbb-a8b9-407a-8a7f-cdce8ddf1955","OnlineTraits":["pistol"],"Category":null},{"RepositoryId":"6561a437-86ef-4338-a01f-005b3476be20","InstanceId":"5400e831-bdf3-4546-aa23-e49d6ddda2cd","OnlineTraits":["explosive","explosive_device"],"Category":null},{"RepositoryId":"1d4f5a7c-c0fb-4d66-9e77-35ae526ef83a","InstanceId":"ceb21ff9-684f-4ace-bcaa-82649c6c2395","OnlineTraits":["tool"],"Category":null},{"RepositoryId":"f3f8ac31-195d-4701-9e7c-775d621a405a","InstanceId":"","OnlineTraits":["NONE"],"Category":null}],"Disguise":"874c4c48-0a8b-49e9-883e-49fc5f1fb051","LocationId":"LOCATION_PARIS","GameChangers":[],"ContractType":"mission","DifficultyLevel":2.0,"IsVR":false,"IsHitmanSuit":true,"SelectedCharacterId":"00000000-0000-0000-0000-000000000000"},"XboxGameMode":3.0,"XboxDifficulty":0.0,"Timestamp":0.0,"Origin":"gameclient","Id":"ca59404d-4f73-4f7b-a148-290591aca972"})json"},
        {190, 196, 146010, R"json({"Name":"ContractFailed","ContractSessionId":"2516109628137904204-c00b2d17-08b1-4949-9f9d-5f68b691f40f","ContractId":"00000000-0000-0000-0000-000000000200","Value":"Contract ended manually: OnRestartLevel","XboxGameMode":3.0,"XboxDifficulty":0.0,"Timestamp":907.95282,"Origin":"gameclient","Id":"39c16512-dad5-4a90-8a9e-4a14120e63bb"})json"},
        {193, 199, 146568, R"json({"Name":"ContractStart","ContractSessionId":"2516109618691980006-9666b5ad-6a4f-44bb-b5fb-ff86bb3a8d76","ContractId":"00000000-0000-0000-0000-000000000200","Value":{"Loadout":[{"RepositoryId":"e70adb5b-0646-4f88-bd4a-85bea7a2a654","InstanceId":"9a3f1dbb-a8b9-407a-8a7f-cdce8ddf1955","OnlineTraits":["pistol"],"Category":null},{"RepositoryId":"6561a437-86ef-4338-a01f-005b3476be20","InstanceId":"5400e831-bdf3-4546-aa23-e49d6ddda2cd","OnlineTraits":["explosive","explosive_device"],"Category":null},{"RepositoryId":"1d4f5a7c-c0fb-4d66-9e77-35ae526ef83a","InstanceId":"ceb21ff9-684f-4ace-bcaa-82649c6c2395","OnlineTraits":["tool"],"Category":null},{"RepositoryId":"f3f8ac31-195d-4701-9e7c-775d621a405a","InstanceId":"","OnlineTraits":["NONE"],"Category":null}],"Disguise":"874c4c48-0a8b-49e9-883e-49fc5f1fb051","LocationId":"LOCATION_PARIS","GameChangers":[],"ContractType":"mission","DifficultyLevel":2.0,"IsVR":false,"IsHitmanSuit":true,"SelectedCharacterId":"00000000-0000-0000-0000-000000000000"},"XboxGameMode":3.0,"XboxDifficulty":0.0,"Timestamp":0.0,"Origin":"gameclient","Id":"f5c89f5e-d2ef-4c9a-9221-c19c7c06f795"})json"},
        {204, 211, 158948, R"json({"Name":"ContractFailed","ContractSessionId":"2516109618691980006-9666b5ad-6a4f-44bb-b5fb-ff86bb3a8d76","ContractId":"00000000-0000-0000-0000-000000000200","Value":"Contract ended manually: User pressed exit to Main menu","XboxGameMode":3.0,"XboxDifficulty":0.0,"Timestamp":105.089394,"Origin":"gameclient","Id":"0e2833dc-9360-4529-939e-906e63208b21"})json"},
    };

    inline constexpr size_t k_ContractLifecycleCount = sizeof(k_ContractLifecycle) / sizeof(k_ContractLifecycle[0]);

    // Indices into k_ContractLifecycle, by role in the B0 session.
    inline constexpr size_t k_ContractStartA = 0;   // fresh load, session A
    inline constexpr size_t k_ContractFailedRestartA = 1; // "Contract ended manually: OnRestartLevel"
    inline constexpr size_t k_ContractStartB = 2;   // after the restart, session B
    inline constexpr size_t k_ContractFailedExitB = 3;    // "Contract ended manually: User pressed exit to Main menu"
}
