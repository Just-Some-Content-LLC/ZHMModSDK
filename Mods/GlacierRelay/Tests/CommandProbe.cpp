// Standalone Windows command probe (M5-B): hosts the real native command link, queue and frame
// scheduler without the game, driven by a fake frame source that advances at a controlled
// interval. It connects to BEAM's command listener (127.0.0.1:<port>, through WSL2's localhost
// forwarding like the DLL does for telemetry), speaks the hello/welcome handshake, receives
// commands, schedules relay.probe once per fake frame and writes the results — exactly the code
// the DLL will run, with the engine replaced by a scripted SceneState. No engine operation, no
// HITMAN. The probe never claims anything about the game: its read-back is the fake frame's.
//
//   GlacierRelayCommandProbe <port> <frame_interval_ms> <duration_ms> [scene]
//
// scene: "menu" (default; scene available, not playing), "mission" (available, playing, with a
// fake mission generation), "unavailable" (scene state not readable). Exit code 0 when the link
// connected and was welcomed at least once.

#include <Windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "CommandLink.h"
#include "CommandQueue.h"
#include "CommandScheduler.h"
#include "RelayAdapter.h"
#include "RelayLog.h"

namespace
{
    uint64_t MonoMs()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
}

int main(int p_Argc, char** p_Argv)
{
    if (p_Argc < 4)
    {
        std::printf("usage: GlacierRelayCommandProbe <port> <frame_interval_ms> <duration_ms> [menu|mission|unavailable]\n");
        return 2;
    }

    const uint16_t s_Port = static_cast<uint16_t>(std::atoi(p_Argv[1]));
    const uint32_t s_Interval = static_cast<uint32_t>(std::atoi(p_Argv[2]));
    const uint32_t s_Duration = static_cast<uint32_t>(std::atoi(p_Argv[3]));
    const std::string s_SceneMode = p_Argc > 4 ? p_Argv[4] : "menu";

    const std::string s_Adapter = RelayAdapter::NewInstanceId();
    RelayLog::Info("command probe: adapter instance {}, port {}, frame interval {} ms, duration {} ms, scene {}, main thread {}", s_Adapter, s_Port, s_Interval, s_Duration, s_SceneMode, GetCurrentThreadId());
    std::printf("command probe: adapter instance %s, port %u, frame interval %u ms, duration %u ms, scene %s, main thread %lu\n", s_Adapter.c_str(), s_Port, s_Interval, s_Duration, s_SceneMode.c_str(), GetCurrentThreadId());

    CommandQueue s_Queue;
    CommandLink::Options s_Options;
    s_Options.port = s_Port;
    s_Options.backoff_initial_ms = 500;
    s_Options.backoff_max_ms = 2000;
    CommandLink s_Link(s_Options, s_Queue, s_Adapter, MonoMs);
    CommandScheduler s_Scheduler(s_Queue, [&](const CommandProtocol::Result& p_Result) {
        RelayLog::Info("command probe: result {} {} frame {}", p_Result.command_id, CommandProtocol::StatusText(p_Result.status), p_Result.frame ? *p_Result.frame : 0);
        s_Link.PushResult(p_Result);
    });

    // The fake frame source: a thread of its own, advancing at the interval, like a frame update
    // that is not the main thread. Its id is what the scheduler records.
    std::atomic<bool> s_Stop{false};
    uint64_t s_Frames = 0;

    std::thread s_FrameThread([&] {
        RelayLog::Info("command probe: fake frame thread {}", GetCurrentThreadId());
        std::printf("command probe: fake frame thread %lu\n", GetCurrentThreadId());
        uint64_t s_Frame = 0;

        while (!s_Stop)
        {
            ++s_Frame;
            CommandScheduler::FrameObservation s_Observation;
            s_Observation.frame = s_Frame;
            s_Observation.now_mono_ms = MonoMs();
            s_Observation.native_at = RelayAdapter::UtcNow();
            s_Observation.thread_id = GetCurrentThreadId();
            s_Observation.link_generation = s_Link.LinkGeneration();
            s_Observation.adapter_instance_id = s_Adapter;

            if (s_SceneMode == "unavailable")
            {
                s_Observation.scene_available = false;
            }
            else
            {
                s_Observation.scene_available = true;
                s_Observation.scene.available = true;
                const bool s_Mission = s_SceneMode == "mission";
                s_Observation.scene.scene_resource = s_Mission ? "assembly:/_PRO/Scenes/Missions/Paris/_Scene_FashionShowHit_01.entity" : "assembly:/_PRO/Scenes/Frontend/MainMenu.entity";
                s_Observation.scene.scene_type = s_Mission ? "mission" : "";
                s_Observation.scene.codename_hint = s_Mission ? "Peacock" : "";
                s_Observation.scene.loading_stage = 8;
                s_Observation.scene.scene_loaded = true;
                s_Observation.mission_playing = s_Mission;
                if (s_Mission)
                    s_Observation.mission_generation = "fake-session-" + s_Adapter.substr(0, 8);
            }

            s_Link.SetMissionGeneration(s_Observation.mission_generation);
            s_Scheduler.Tick(s_Observation);
            s_Frames = s_Frame;
            std::this_thread::sleep_for(std::chrono::milliseconds(s_Interval));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(s_Duration));
    s_Stop = true;
    s_FrameThread.join();

    const auto s_Stats = s_Link.GetStats();
    const auto s_Queued = s_Queue.GetStats();
    const auto s_Scheduled = s_Scheduler.GetStats();

    std::printf(
        "command probe: link connects %llu disconnects %llu refusals %llu generation %llu; lines %llu queued %llu busy %llu duplicate %llu rejected %llu violations %llu results_sent %llu cleared %llu; network thread %lu\n",
        s_Stats.connects, s_Stats.disconnects, s_Stats.refusals, s_Stats.link_generation, s_Stats.lines_received, s_Stats.commands_queued, s_Stats.commands_busy, s_Stats.commands_duplicate, s_Stats.commands_rejected, s_Stats.protocol_violations, s_Stats.results_sent, s_Stats.cleared_on_disconnect, s_Stats.network_thread_id
    );
    std::printf(
        "command probe: queue pushed %llu popped %llu busy %llu duplicates %llu cleared %llu remembered %zu; scheduler frames %llu executed %llu expired %llu stale %llu rejected %llu first thread %lu last thread %lu changed %d last latency %llu ms max %llu ms\n",
        s_Queued.pushed, s_Queued.popped, s_Queued.busy, s_Queued.duplicates, s_Queued.cleared, s_Queued.remembered, s_Scheduled.frames, s_Scheduled.executed, s_Scheduled.expired, s_Scheduled.stale_generation, s_Scheduled.rejected, s_Scheduled.first_thread_id, s_Scheduled.last_thread_id, s_Scheduled.thread_changed ? 1 : 0, s_Scheduled.last_latency_ms, s_Scheduled.max_latency_ms
    );
    RelayLog::Info("command probe: done after {} fake frames; connects {}, executed {}, expired {}, violations {}", s_Frames, s_Stats.connects, s_Scheduled.executed, s_Scheduled.expired, s_Stats.protocol_violations);

    return s_Stats.connects > 0 ? 0 : 1;
}
