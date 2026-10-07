// Standalone Windows client for the Windows -> WSL2 -> BEAM integration test. Drives the real
// engine-independent pipeline (MissionObserver -> RelayAdapter -> TcpRelaySink) without the game.
//
//   GlacierRelayWireProbe <port> <step>[,<step>...]
//
// Steps: "stage1" replays the scene sequence recorded in the M1 stage 1 runtime experiment
// through MissionObserver (since M2: 6 events, a mission.playing/mission.stopped pair per mission
// entry); "publish" publishes one mission.playing directly; "stop" publishes one mission.stopped
// directly; "sleep:<ms>" waits. Exit code is 0 when the adapter's sequence count matches what was
// asked.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "Fixtures/B0ActorOutcomes.h"
#include "MissionObserver.h"
#include "RelayAdapter.h"
#include "RelayLog.h"
#include "TcpRelaySink.h"
#include "TelemetryNormalizer.h"
#include "TestJson.h"

namespace
{
    SceneState Scene(const char* p_Type, int32_t p_Stage, bool p_Loaded, const char* p_Resource, const char* p_Hint)
    {
        SceneState s_Scene;
        s_Scene.available = true;
        s_Scene.scene_resource = p_Resource;
        s_Scene.scene_type = p_Type;
        s_Scene.codename_hint = p_Hint;
        s_Scene.loading_stage = p_Stage;
        s_Scene.scene_loaded = p_Loaded;
        return s_Scene;
    }

    int ReplayStage1(RelayAdapter& p_Adapter)
    {
        const char* s_Menu = "assembly:/_PRO/Scenes/Frontend/MainMenu.entity";
        const char* s_Paris = "assembly:/_PRO/Scenes/Missions/Paris/_Scene_FashionShowHit_01.entity";
        const char* s_Sapienza = "assembly:/_PRO/Scenes/Missions/CoastalTown/Mission01.entity";

        struct Step { SceneState Scene; const char* SessionId; };
        const std::vector<Step> s_Steps = {
            {Scene("", 5, true, s_Menu, ""), nullptr}, {Scene("", 8, true, s_Menu, ""), nullptr},
            {Scene("", 8, false, s_Menu, ""), nullptr}, {Scene("", 2, false, s_Menu, ""), nullptr},
            {Scene("mission", 5, false, s_Paris, "Peacock"), nullptr}, {Scene("mission", 7, false, s_Paris, "Peacock"), nullptr},
            {Scene("mission", 8, true, s_Paris, "Peacock"), "2516109767498002969-53bb690f-b7c0-400f-b41a-7e9200d66078"},
            {Scene("mission", 8, false, s_Paris, "Peacock"), nullptr}, {Scene("mission", 0, false, s_Paris, "Peacock"), nullptr},
            {Scene("mission", 7, false, s_Paris, "Peacock"), nullptr},
            {Scene("mission", 8, true, s_Paris, "Peacock"), "2516109766730159341-60aeb67e-b1d8-4e9a-9b7e-000000000002"},
            {Scene("mission", 8, false, s_Paris, "Peacock"), nullptr}, {Scene("", 8, true, s_Menu, ""), nullptr},
            {Scene("", 2, false, s_Menu, ""), nullptr}, {Scene("mission", 7, true, s_Sapienza, "Octopus"), nullptr},
            {Scene("mission", 8, true, s_Sapienza, "Octopus"), "2516109765960127603-0277839e-7a1c-4c53-8d1f-000000000003"},
            {Scene("mission", 8, false, s_Sapienza, "Octopus"), nullptr}, {Scene("", 8, true, s_Menu, ""), nullptr},
        };

        MissionObserver s_Observer;
        int s_Published = 0;

        for (const auto& s_Step : s_Steps)
        {
            std::optional<std::string> s_SessionId;
            if (s_Step.SessionId)
                s_SessionId = s_Step.SessionId;

            if (const auto s_Event = s_Observer.Update(s_Step.Scene, s_SessionId))
            {
                p_Adapter.Publish(*s_Event);
                ++s_Published;
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }

        return s_Published;
    }
}

int main(int p_Argc, char** p_Argv)
{
    if (p_Argc < 3)
    {
        std::printf("usage: GlacierRelayWireProbe <port> <step>[,<step>...]\n");
        return 2;
    }

    TcpRelaySink::Options s_Options;
    s_Options.port = static_cast<uint16_t>(std::atoi(p_Argv[1]));

    RelayAdapter s_Adapter(std::make_unique<TcpRelaySink>(s_Options), RelayAdapter::NewInstanceId(), &RelayAdapter::UtcNow);
    std::printf("wire probe: adapter instance %s -> 127.0.0.1:%u, log %s\n", s_Adapter.InstanceId().c_str(), s_Options.port, RelayLog::Path().c_str());

    int s_Expected = 0;
    std::string s_Script = p_Argv[2];
    size_t s_Pos = 0;

    while (s_Pos <= s_Script.size())
    {
        const size_t s_Comma = s_Script.find(',', s_Pos);
        const std::string s_Step = s_Script.substr(s_Pos, s_Comma == std::string::npos ? std::string::npos : s_Comma - s_Pos);
        s_Pos = s_Comma == std::string::npos ? s_Script.size() + 1 : s_Comma + 1;

        if (s_Step == "stage1")
        {
            s_Expected += ReplayStage1(s_Adapter);
        }
        else if (s_Step == "publish")
        {
            MissionPlayingEvent s_Event;
            s_Event.scene_resource = "assembly:/_PRO/Scenes/Missions/Paris/_Scene_FashionShowHit_01.entity";
            s_Event.scene_type = "mission";
            s_Event.codename_hint = "Peacock";
            s_Adapter.Publish(s_Event);
            ++s_Expected;
        }
        else if (s_Step == "b1")
        {
            TelemetryNormalizer s_Normalizer;

            for (size_t i = 0; i < B0Fixtures::k_ActorOutcomeCount; ++i)
            {
                const auto s_Result = s_Normalizer.Normalize(
                    TestJson::ObservationFromRecordedEvent(B0Fixtures::k_ActorOutcomes[i].json, static_cast<uint32_t>(i + 1))
                );

                if (s_Result.outcome != TelemetryNormalizer::Outcome::Normalized)
                {
                    std::printf("b1: fixture %zu did not normalize (%s)\n", i, s_Result.detail.c_str());
                    return 3;
                }

                s_Adapter.Publish(*s_Result.event);
                ++s_Expected;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        else if (s_Step == "stop")
        {
            MissionStoppedEvent s_Event;
            s_Event.scene_resource = "assembly:/_PRO/Scenes/Missions/Paris/_Scene_FashionShowHit_01.entity";
            s_Event.scene_type = "mission";
            s_Event.codename_hint = "Peacock";
            s_Adapter.Publish(s_Event);
            ++s_Expected;
        }
        else if (s_Step.rfind("sleep:", 0) == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(s_Step.c_str() + 6)));
        }
        else if (!s_Step.empty())
        {
            std::printf("unknown step '%s'\n", s_Step.c_str());
            return 2;
        }
    }

    // Let the sender thread drain before the sink is destroyed.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::printf("wire probe: published %llu envelope(s), expected %d\n", static_cast<unsigned long long>(s_Adapter.PublishedCount()), s_Expected);
    return s_Adapter.PublishedCount() == static_cast<uint64_t>(s_Expected) ? 0 : 1;
}
