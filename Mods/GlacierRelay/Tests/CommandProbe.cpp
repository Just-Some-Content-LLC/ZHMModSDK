// Standalone Windows command probe (M5-B, extended in M5 C-1 and C-2B): hosts the real native
// command link, queue, frame scheduler and apply state machine without the game, driven by a fake
// frame source that advances at a controlled interval. It connects to BEAM's command listener
// (127.0.0.1:<port>, through WSL2's localhost forwarding like the DLL does for telemetry), speaks
// the hello/welcome handshake (version 1, or version 2 with the attestation when GLACIER_PROBE_TOKEN
// is set in the environment), receives commands, schedules them once per fake frame and writes the
// results — exactly the code the DLL will run, with the engine replaced by a scripted SceneState, a
// FAKE inspection source and a FAKE outfit applier. No engine operation, no HITMAN, no SDK function:
// every value the probe's inspector or applier reports is scripted here and is never an observation
// of HITMAN; the DLL's SceneObservation and OutfitApplier are not linked into this executable.
//
//   GlacierRelayCommandProbe <port> <frame_interval_ms> <duration_ms> [scene] [inert|effectful] [apply-mode]
//
// scene: "menu" (default; scene available, not playing, no hero), "mission" (available, playing,
// a fake generation, a fake hero outfit and a fake kit table), "unavailable" (scene state not
// readable), "restart" (as mission, then from fake frame 8 the fake registry names the next
// generation while the predicate still holds — the race M5-B observed — the predicate falls at
// frame 12 and rises again at frame 16 with the new generation).
// inert|effectful: whether the link advertises and decodes player.outfit.set/2 and the frame source
// carries the fake applier (the DLL's [command] effectful setting). Default inert.
// apply-mode (effectful only): "ok" (the fake setter changes the fake tuple; default), "unchanged",
// "elsewhere", "fault" (the fake setter reports a fault; the real guard would fail-stop instead),
// "readback-missing", "post-generation".
// Exit code 0 when the link connected and was welcomed at least once.

#include <Windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "CommandLink.h"
#include "CommandQueue.h"
#include "CommandScheduler.h"
#include "FakeOutfitApplier.h"
#include "RelayAdapter.h"
#include "RelayLog.h"

namespace
{
    uint64_t MonoMs()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    // The fake kit table: ids chosen to look like repository ids and labelled fake everywhere.
    constexpr const char* k_FakeSuitId = FakeOutfit::World::k_FakeSuit;        // the id the real runs showed as the starting suit; here a FAKE table entry
    constexpr const char* k_FakeTargetId = FakeOutfit::World::k_FakeTarget;    // the C-1 menu kit id; here a FAKE table entry, never resolved against HITMAN
    constexpr const char* k_FakeOtherKitId = "2018db77-1111-4111-8111-111111111111"; // a fake kit with no HeroA variation
    constexpr const char* k_FakeNoHeroAKitId = FakeOutfit::World::k_FakeNoHeroA;   // a fake kit whose charset has no HeroA collection

    struct FakeWorld
    {
        bool playing = false;
        bool hero_present = false;
        std::string registry_generation; // what the fake registry reports (may differ from the cached one)
        bool setter_available = true;
    };

    OutfitObservation::EngineReads FakeInspect(const FakeWorld& p_World, const FakeOutfit::World& p_Outfit, const std::optional<std::string>& p_CandidateId)
    {
        using OutfitObservation::Field;
        using CommandProtocol::FieldStatus;
        OutfitObservation::EngineReads s_Reads;
        s_Reads.thread_id = GetCurrentThreadId();

        s_Reads.generation_fresh = p_World.registry_generation.empty()
            ? Field<std::string>::Of(FieldStatus::Unavailable, "fake registry session id empty")
            : Field<std::string>::Observed(p_World.registry_generation);

        if (p_World.hero_present)
        {
            s_Reads.hero.player_resolved = true;
            s_Reads.hero.outfit_id = Field<std::string>::Observed(p_Outfit.hero_outfit_id);
            s_Reads.hero.charset = Field<int64_t>::Observed(p_Outfit.hero_charset);
            s_Reads.hero.variation = Field<int64_t>::Observed(p_Outfit.hero_variation);
            s_Reads.hero.charset_count = Field<int64_t>::Observed(1);
            s_Reads.hero.heroa_variations = Field<int64_t>::Observed(1);
            s_Reads.hero.heroa_valid = Field<bool>::Observed(true);
            s_Reads.hero.is_hitman_suit = Field<bool>::Observed(true);
        }
        else
        {
            s_Reads.hero.outfit_id = Field<std::string>::Of(FieldStatus::Unavailable, "fake hero entity reference null");
            s_Reads.hero.heroa_valid = Field<bool>::Of(FieldStatus::Unavailable, "fake hero entity reference null");
        }

        s_Reads.kit_map_available = Field<bool>::Observed(true);

        if (p_CandidateId)
        {
            OutfitObservation::KitResolution s_Kit;
            s_Kit.kits_scanned = Field<int64_t>::Observed(4);

            if (*p_CandidateId == k_FakeSuitId || *p_CandidateId == k_FakeTargetId)
            {
                s_Kit.found = Field<bool>::Observed(true);
                s_Kit.charsets = Field<int64_t>::Observed(1);
                s_Kit.heroa_variations = Field<int64_t>::Observed(1);
                s_Kit.heroa_usable = Field<bool>::Observed(true);
                s_Kit.is_hitman_suit = Field<bool>::Observed(true);
                s_Kit.charset_heroa_variations = {Field<int64_t>::Observed(1)};
            }
            else if (*p_CandidateId == k_FakeOtherKitId)
            {
                s_Kit.found = Field<bool>::Observed(true);
                s_Kit.charsets = Field<int64_t>::Observed(2);
                s_Kit.heroa_variations = Field<int64_t>::Observed(0);
                s_Kit.heroa_usable = Field<bool>::Observed(false);
                s_Kit.is_hitman_suit = Field<bool>::Observed(false);
                s_Kit.charset_heroa_variations = {Field<int64_t>::Observed(0), Field<int64_t>::Observed(0)};
            }
            else if (*p_CandidateId == k_FakeNoHeroAKitId)
            {
                s_Kit.found = Field<bool>::Observed(true);
                s_Kit.charsets = Field<int64_t>::Observed(1);
                s_Kit.heroa_variations = Field<int64_t>::Of(FieldStatus::Unavailable, "no HeroA character type in charset");
                s_Kit.heroa_usable = Field<bool>::Observed(false);
                s_Kit.is_hitman_suit = Field<bool>::Observed(false);
                s_Kit.charset_heroa_variations = {Field<int64_t>::Of(FieldStatus::Unavailable, "no HeroA character type in charset")};
            }
            else
            {
                s_Kit.found = Field<bool>::Observed(false);
            }

            s_Reads.candidate = s_Kit;
        }

        s_Reads.set_outfit_available = Field<bool>::Observed(p_World.setter_available);
        return s_Reads;
    }

    // The probe's fake applier shares the fake world's hero tuple with the fake inspector, so an
    // inspection after a fake apply reports the fake tuple the fake setter left.
    class ProbeApplier final : public OutfitApply::IOutfitApplier
    {
    public:
        FakeOutfit::Applier inner;        // the scripted state machine the tests use
        const FakeWorld* world = nullptr; // the frame's scripted scene (generation, hero presence)

        OutfitObservation::EngineReads Observe(const std::optional<std::string>& p_CandidateId) override
        {
            inner.world.player_present = world && world->hero_present;
            inner.world.fresh_generation = world ? world->registry_generation : "";
            inner.world.post_generation = inner.world.fresh_generation;
            inner.world.setter_available = world && world->setter_available;
            return inner.Observe(p_CandidateId);
        }

        OutfitApply::CallOutcome Invoke(int32_t p_Charset, int32_t p_Variation) override
        {
            const auto s_Outcome = inner.Invoke(p_Charset, p_Variation);
            RelayLog::Info("command probe: FAKE setter invoked (charset {}, variation {}); called {} faulted {} — simulated, no engine", p_Charset, p_Variation, s_Outcome.called, s_Outcome.faulted);
            std::printf("command probe: FAKE setter invoked (charset %d, variation %d); called %d faulted %d (simulated; no engine)\n", p_Charset, p_Variation, s_Outcome.called ? 1 : 0, s_Outcome.faulted ? 1 : 0);
            return s_Outcome;
        }

        OutfitObservation::EngineReads ReadBack() override { return inner.ReadBack(); }
        bool Latched() const override { return inner.Latched(); }
        void Release() override { inner.Release(); }
    };
}

int main(int p_Argc, char** p_Argv)
{
    if (p_Argc < 4)
    {
        std::printf("usage: GlacierRelayCommandProbe <port> <frame_interval_ms> <duration_ms> [menu|mission|unavailable|restart] [inert|effectful] [ok|unchanged|elsewhere|fault|readback-missing|post-generation]\n");
        return 2;
    }

    const uint16_t s_Port = static_cast<uint16_t>(std::atoi(p_Argv[1]));
    const uint32_t s_Interval = static_cast<uint32_t>(std::atoi(p_Argv[2]));
    const uint32_t s_Duration = static_cast<uint32_t>(std::atoi(p_Argv[3]));
    const std::string s_SceneMode = p_Argc > 4 ? p_Argv[4] : "menu";
    const bool s_Effectful = p_Argc > 5 && std::string(p_Argv[5]) == "effectful";
    const std::string s_ApplyMode = p_Argc > 6 ? p_Argv[6] : "ok";

    // The test token comes from the environment only (never a file, never an argument that a
    // transcript would show); it is never printed.
    std::string s_Token;
    {
        char s_Buffer[128] = {};
        const DWORD s_Len = GetEnvironmentVariableA("GLACIER_PROBE_TOKEN", s_Buffer, sizeof(s_Buffer));
        if (s_Len > 0 && s_Len < sizeof(s_Buffer))
            s_Token.assign(s_Buffer, s_Len);
    }

    const std::string s_Adapter = RelayAdapter::NewInstanceId();
    RelayLog::Info("command probe: adapter instance {}, port {}, frame interval {} ms, duration {} ms, scene {}, {} , apply-mode {}, token {}, main thread {} (FAKE frame source, FAKE inspection data and FAKE applier; never HITMAN state)", s_Adapter, s_Port, s_Interval, s_Duration, s_SceneMode, s_Effectful ? "effectful" : "inert", s_ApplyMode, s_Token.empty() ? "absent (protocol version 1)" : "present (protocol version 2; not logged)", GetCurrentThreadId());
    std::printf("command probe: adapter instance %s, port %u, frame interval %u ms, duration %u ms, scene %s, %s, apply-mode %s, token %s, main thread %lu (fake frame source, fake inspection data and fake applier)\n", s_Adapter.c_str(), s_Port, s_Interval, s_Duration, s_SceneMode.c_str(), s_Effectful ? "effectful" : "inert", s_ApplyMode.c_str(), s_Token.empty() ? "absent (v1)" : "present (v2; not printed)", GetCurrentThreadId());

    // The stable rule scaled to the fake frame rate (the real rule is 1,000 ms and 60 frames):
    // 3 fake frames and 3 intervals, stated in the output so no reader mistakes it for the DLL's.
    PreconditionEvaluator::Rules s_Rules;
    s_Rules.min_stable_frames = 3;
    s_Rules.min_stable_ms = static_cast<uint64_t>(s_Interval) * 3;
    std::printf("command probe: precondition stable rule scaled for the fake frame rate: %llu ms, %llu frames (the DLL uses 1000 ms, 60 frames)\n", s_Rules.min_stable_ms, s_Rules.min_stable_frames);

    ProbeApplier s_Applier;
    if (s_ApplyMode == "unchanged") s_Applier.inner.world.mode = FakeOutfit::Mode::Unchanged;
    else if (s_ApplyMode == "elsewhere") s_Applier.inner.world.mode = FakeOutfit::Mode::Elsewhere;
    else if (s_ApplyMode == "fault") s_Applier.inner.world.mode = FakeOutfit::Mode::FaultDuringCall;
    else if (s_ApplyMode == "readback-missing") s_Applier.inner.world.mode = FakeOutfit::Mode::ReadBackUnavailable;
    else if (s_ApplyMode == "post-generation") s_Applier.inner.world.mode = FakeOutfit::Mode::PostGenerationChange;

    CommandQueue s_Queue;
    CommandLink::Options s_Options;
    s_Options.port = s_Port;
    s_Options.backoff_initial_ms = 500;
    s_Options.backoff_max_ms = 2000;
    s_Options.effectful = s_Effectful;
    s_Options.token = s_Token;
    CommandLink s_Link(s_Options, s_Queue, s_Adapter, MonoMs);
    CommandScheduler s_Scheduler(s_Queue, [&](const CommandProtocol::Result& p_Result) {
        RelayLog::Info("command probe: result {} {} frame {} ({})", p_Result.command_id, CommandProtocol::StatusText(p_Result.status), p_Result.frame ? *p_Result.frame : 0, p_Result.reason);
        std::printf("command probe: result %s %s frame %llu (%s)\n", p_Result.command_id.c_str(), CommandProtocol::StatusText(p_Result.status), p_Result.frame ? *p_Result.frame : 0ull, p_Result.reason.c_str());
        s_Link.PushResult(p_Result);
    }, s_Rules);

    // The fake frame source: a thread of its own, advancing at the interval, like a frame update
    // that is not the main thread. Its id is what the scheduler records. It maintains the same
    // transition instrumentation the plugin does, over the scripted world.
    std::atomic<bool> s_Stop{false};
    uint64_t s_Frames = 0;

    std::thread s_FrameThread([&] {
        RelayLog::Info("command probe: fake frame thread {}", GetCurrentThreadId());
        std::printf("command probe: fake frame thread %lu\n", GetCurrentThreadId());
        uint64_t s_Frame = 0;
        uint64_t s_Epoch = 0;
        uint64_t s_RiseFrame = 0, s_RiseMono = 0;
        bool s_PrevPlaying = false;
        bool s_RegistryChangeAdvanced = false;
        std::optional<std::string> s_Cached;
        std::optional<std::string> s_ContractStarted, s_ContractEnded;
        const std::string s_Generation1 = "fake-session-" + s_Adapter.substr(0, 8) + "-1";
        const std::string s_Generation2 = "fake-session-" + s_Adapter.substr(0, 8) + "-2";
        const bool s_Mission = s_SceneMode == "mission" || s_SceneMode == "restart";
        FakeWorld s_World;
        s_Applier.world = &s_World;

        auto s_Advance = [&](const char* p_Why) {
            ++s_Epoch;
            RelayLog::Info("command probe: fake relay transition epoch {}: {} (fake frame {})", s_Epoch, p_Why, s_Frame);
            std::printf("command probe: fake relay transition epoch %llu: %s (fake frame %llu)\n", s_Epoch, p_Why, s_Frame);
        };

        while (!s_Stop)
        {
            ++s_Frame;

            // The scripted world for this frame.
            bool s_Playing = s_Mission;
            std::string s_Registry = s_Mission ? s_Generation1 : "";
            if (s_SceneMode == "restart")
            {
                if (s_Frame >= 8 && s_Frame < 12) { s_Playing = true; s_Registry = s_Generation2; }   // the race: registry ahead of the predicate
                else if (s_Frame >= 12 && s_Frame < 16) { s_Playing = false; s_Registry = s_Generation2; }
                else if (s_Frame >= 16) { s_Playing = true; s_Registry = s_Generation2; }
            }
            s_World.playing = s_Playing;
            s_World.hero_present = s_Playing;
            s_World.registry_generation = s_Registry;

            // Edges and contract lifecycle as the plugin would see them (scripted).
            if (s_Playing && !s_PrevPlaying)
            {
                s_Cached = s_Registry;
                s_Advance("mission predicate rise");
                s_RiseFrame = s_Frame;
                s_RiseMono = MonoMs();
                s_RegistryChangeAdvanced = false;
                s_ContractStarted = s_Registry; // contract.started observed right after the rise
            }
            else if (!s_Playing && s_PrevPlaying)
            {
                s_Advance("mission predicate fall");
                s_Cached.reset();
            }
            if (s_SceneMode == "restart" && s_Frame == 8)
            {
                s_ContractEnded = s_Generation1; // contract.ended for the active generation (before the fall)
                s_Advance("contract end observed for the active generation");
            }

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
                s_Observation.scene.scene_resource = s_Mission ? "assembly:/_PRO/Scenes/Missions/Paris/_Scene_FashionShowHit_01.entity" : "assembly:/_PRO/Scenes/Frontend/MainMenu.entity";
                s_Observation.scene.scene_type = s_Playing ? "mission" : (s_Mission ? "mission" : "");
                s_Observation.scene.codename_hint = s_Mission ? "Peacock" : "";
                s_Observation.scene.loading_stage = s_Playing ? 8 : (s_Mission ? 5 : 8);
                s_Observation.scene.scene_loaded = s_Playing || !s_Mission;
                s_Observation.mission_playing = s_Playing;
                s_Observation.mission_generation = s_Cached;
            }

            s_Observation.relay_transition_epoch = s_Epoch;
            s_Observation.previous_frame_playing = s_PrevPlaying;
            s_Observation.rise_frame = s_RiseFrame;
            s_Observation.rise_mono_ms = s_RiseMono;
            s_Observation.scene_resource_same_as_rise = s_Playing; // the scripted resource never changes
            s_Observation.contract_started_seen = s_ContractStarted;
            s_Observation.contract_ended_seen = s_ContractEnded;
            s_Observation.inspector = [&](const std::optional<std::string>& p_Candidate) {
                auto s_Reads = FakeInspect(s_World, s_Applier.inner.world, p_Candidate);
                if (s_Cached && s_Reads.generation_fresh.observed() && *s_Reads.generation_fresh.value != *s_Cached && !s_RegistryChangeAdvanced)
                {
                    s_RegistryChangeAdvanced = true;
                    s_Advance("registry generation differs from the edge-cached generation on an inspection frame");
                }
                return s_Reads;
            };
            s_Observation.applier = s_Effectful ? &s_Applier : nullptr;
            s_Observation.link_generation_now = [&] { return s_Link.LinkGeneration(); };

            s_Link.SetMissionGeneration(s_Observation.mission_generation);
            s_Scheduler.Tick(s_Observation);
            s_PrevPlaying = s_Playing;
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
        "command probe: link version %u effectful %d connects %llu disconnects %llu refusals %llu attests %llu generation %llu; lines %llu queued %llu busy %llu duplicate %llu rejected %llu violations %llu results_sent %llu cleared %llu; network thread %lu\n",
        s_Stats.protocol_version, s_Stats.effectful ? 1 : 0, s_Stats.connects, s_Stats.disconnects, s_Stats.refusals, s_Stats.attests_sent, s_Stats.link_generation, s_Stats.lines_received, s_Stats.commands_queued, s_Stats.commands_busy, s_Stats.commands_duplicate, s_Stats.commands_rejected, s_Stats.protocol_violations, s_Stats.results_sent, s_Stats.cleared_on_disconnect, s_Stats.network_thread_id
    );
    std::printf(
        "command probe: queue pushed %llu popped %llu busy %llu duplicates %llu cleared %llu remembered %zu; scheduler frames %llu executed %llu inspected %llu outfit dispatched %llu called %llu changed %llu not_applied %llu uncertain %llu reduced %llu expired %llu stale %llu rejected %llu first thread %lu last thread %lu changed %d last latency %llu ms max %llu ms; FAKE applier observes %d invokes %d readbacks %d latched %d\n",
        s_Queued.pushed, s_Queued.popped, s_Queued.busy, s_Queued.duplicates, s_Queued.cleared, s_Queued.remembered, s_Scheduled.frames, s_Scheduled.executed, s_Scheduled.inspected, s_Scheduled.outfit_dispatched, s_Scheduled.outfit_called, s_Scheduled.outfit_changed, s_Scheduled.outfit_not_applied, s_Scheduled.outfit_uncertain, s_Scheduled.reduced, s_Scheduled.expired, s_Scheduled.stale_generation, s_Scheduled.rejected, s_Scheduled.first_thread_id, s_Scheduled.last_thread_id, s_Scheduled.thread_changed ? 1 : 0, s_Scheduled.last_latency_ms, s_Scheduled.max_latency_ms,
        s_Applier.inner.observes, s_Applier.inner.invokes, s_Applier.inner.readbacks, s_Applier.inner.Latched() ? 1 : 0
    );
    RelayLog::Info("command probe: done after {} fake frames; connects {}, executed {}, inspected {}, outfit dispatched {} called {} changed {}, expired {}, violations {}", s_Frames, s_Stats.connects, s_Scheduled.executed, s_Scheduled.inspected, s_Scheduled.outfit_dispatched, s_Scheduled.outfit_called, s_Scheduled.outfit_changed, s_Scheduled.expired, s_Stats.protocol_violations);

    return s_Stats.connects > 0 ? 0 : 1;
}
