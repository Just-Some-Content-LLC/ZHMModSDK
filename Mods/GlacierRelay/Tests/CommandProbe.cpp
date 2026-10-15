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
//   GlacierRelayCommandProbe <port> <frame_interval_ms> <duration_ms> [scene] [inert|effectful] [apply-mode] [players-mode]
//
// scene: "menu" (default; scene available, not playing, no hero), "mission" (available, playing,
// a fake generation, a fake hero outfit and a fake kit table), "unavailable" (scene state not
// readable), "restart" (as mission, then from fake frame 8 the fake registry names the next
// generation while the predicate still holds — the race M5-B observed — the predicate falls at
// frame 12 and rises again at frame 16 with the new generation). M5 C-2C adds three scripted
// mission variants for the precondition matrix: "no-contract-start" (as mission, but no
// contract.started is ever observed for the attempt), "contract-ended" (as mission; from fake
// frame 6 a contract.ended for the active generation has been observed while the predicate still
// holds), "registry-ahead" (as mission; from fake frame 6 the fake registry names the next
// generation while the edge-cached generation and the predicate stay as they were, with no fall).
// inert|effectful: whether the link advertises and decodes player.outfit.set/2 and the frame source
// carries the fake applier (the DLL's [command] effectful setting). Default inert.
// apply-mode (effectful only): "ok" (the fake setter changes the fake tuple; default), "unchanged",
// "elsewhere", "fault" (the fake setter reports a fault; the real guard would fail-stop instead),
// "readback-missing", "post-generation". M5 C-2C adds: "setter-unavailable" (the fake setter is
// reported absent, as on a build where the pattern is not found); "exit-after-call" (the process
// terminates abruptly right after the fake setter returned and before the result is written to the
// link — link loss after application, before result delivery); "log-unavailable", "log-failed" and
// "log-short" (the real RelayLog's raw writer is replaced through its test seam by a scripted
// writer that reports the FIRST "applying" line as not accepted — file not open, WriteFile failed,
// or fewer bytes than requested — and accepts every later line; every other line is passed
// through to a probe-owned file, so the production logging contract is exercised unchanged and
// the second effectful command of a scenario proves the refusal does not latch).
// players-mode (M6-B1): the FAKE registry, brick, entity and transform state the two read-only
// inspections report (relay.inspect.players / relay.inspect.entity). "default" (slot 0 occupied with a
// fake hero whenever the scene has one, slots 1-3 empty, 5 fake bricks at the menu and 23 in a
// mission, the hero owned by a fake brick, the round trip returning the hero, the candidate brick
// absent), "registry-unavailable", "layout-unsupported", "extra-slot" (slot 1 occupied too),
// "hero-missing" (slot 0 has an entity reference but no hero interface), "candidate-present" (the
// candidate brick and its authored entity resolve, distinct from the local hero), "roundtrip-different",
// "roundtrip-unrepresentable" (index -1), "factory-mismatch", "subentity-missing", "bricks-empty",
// "bricks-oversized" (70), "dirty-transform", "invalid-flags" (predeleted). Every value is scripted
// here and is never an observation of HITMAN.
// Exit code 0 when the link connected and was welcomed at least once.

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>

#include "CommandLink.h"
#include "CommandQueue.h"
#include "CommandScheduler.h"
#include "FakeOutfitApplier.h"
#include "PlayerObservation.h"
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

    // ---- M6-B1: the FAKE registry and entity state the two read-only inspections report -------------
    // Plain values scripted per players-mode. Never an observation of HITMAN; no pointer exists here.
    constexpr const char* k_FakeHeroEntityId = "feed9a0d70d546f4";     // a FAKE authored id for the local hero
    constexpr const char* k_FakeHeroBrick = "0012e5a7b1c9d3ee";        // a FAKE loaded brick that owns the hero
    constexpr const char* k_FakeHitmenBrick = "007c57a6ea581f8c";      // a FAKE stand-in for the historical brick id
    constexpr const char* k_FakeAgent48 = "feede715906f747f";          // the historical authored id, used as a FAKE lookup key
    constexpr const char* k_FakeCharacterId = "787c1ff2-467c-421d-98f4-6bbb92e5fc99";
    constexpr const char* k_FakeZeros = "00000000-0000-0000-0000-000000000000";

    PlayerObservation::SlotReads FakeEmptySlot()
    {
        using OutfitObservation::Field;
        using CommandProtocol::FieldStatus;
        PlayerObservation::SlotReads s_Slot;
        s_Slot.player_id = Field<int64_t>::Observed(-1);
        s_Slot.local_flag = Field<bool>::Observed(true);
        s_Slot.connected_flag = Field<bool>::Observed(false);
        s_Slot.net_player = Field<bool>::Observed(false);
        s_Slot.replica = Field<bool>::Observed(false);
        s_Slot.entity_present = Field<bool>::Observed(false);
        s_Slot.hero_resolved = Field<bool>::Of(FieldStatus::Unavailable, "entity reference null");
        s_Slot.flags_a0 = Field<int64_t>::Observed(-1);
        s_Slot.session_id = Field<std::string>::Of(FieldStatus::Unavailable, "session id empty");
        s_Slot.outfit_id = Field<std::string>::Observed(k_FakeZeros);
        s_Slot.character_id = Field<std::string>::Observed(k_FakeZeros);
        return s_Slot;
    }

    PlayerObservation::RegistryReads FakePlayers(const FakeWorld& p_World, const FakeOutfit::World& p_Outfit, const std::string& p_Mode)
    {
        using OutfitObservation::Field;
        using CommandProtocol::FieldStatus;

        if (p_Mode == "registry-unavailable")
        {
            auto s_Reads = PlayerObservation::AllRegistry(FieldStatus::Unavailable, "fake registry global null");
            s_Reads.registry_available = Field<bool>::Observed(false);
            s_Reads.thread_id = GetCurrentThreadId();
            return s_Reads;
        }

        if (p_Mode == "layout-unsupported")
        {
            auto s_Reads = PlayerObservation::AllRegistry(FieldStatus::Unsupported, "fake registry layout unsupported");
            s_Reads.registry_available = Field<bool>::Observed(true);
            s_Reads.thread_id = GetCurrentThreadId();
            return s_Reads;
        }

        PlayerObservation::RegistryReads s_Reads;
        s_Reads.thread_id = GetCurrentThreadId();
        s_Reads.registry_available = Field<bool>::Observed(true);
        s_Reads.words = Field<std::string>::Observed("slot0,0,0x4000000000000101");
        for (auto& s_Slot : s_Reads.slots)
            s_Slot = FakeEmptySlot();

        // slot 0: the fake local player (a hero whenever the scripted scene has one; in a mission the
        // registry's session id and outfit guid; at the menu neither, as experiment 4 saw)
        auto& s_Local = s_Reads.slots[0];
        s_Local.player_id = Field<int64_t>::Observed(0);
        s_Local.flags_a0 = Field<int64_t>::Observed(0);
        s_Local.entity_present = Field<bool>::Observed(true);
        s_Local.hero_resolved = Field<bool>::Observed(p_Mode != "hero-missing");
        if (p_World.playing)
        {
            s_Local.session_id = p_World.registry_generation.empty() ? Field<std::string>::Of(FieldStatus::Unavailable, "session id empty") : Field<std::string>::Observed(p_World.registry_generation);
            s_Local.outfit_id = Field<std::string>::Observed(p_Outfit.hero_outfit_id);
        }

        if (p_Mode == "extra-slot")
        {
            auto& s_Extra = s_Reads.slots[1];
            s_Extra.player_id = Field<int64_t>::Observed(2);
            s_Extra.flags_a0 = Field<int64_t>::Observed(0);
            s_Extra.entity_present = Field<bool>::Observed(true);
            s_Extra.hero_resolved = Field<bool>::Observed(true);
            s_Extra.net_player = Field<bool>::Observed(true);
            s_Extra.connected_flag = Field<bool>::Observed(true);
            s_Extra.character_id = Field<std::string>::Observed(k_FakeCharacterId);
        }

        if (p_Mode == "hero-missing")
        {
            s_Reads.hero_kit_id = Field<std::string>::Of(FieldStatus::Unavailable, "hero interface not resolved");
            s_Reads.slot0_matches_m5 = Field<bool>::Of(FieldStatus::Unavailable, "kit id or registry outfit guid unavailable");
        }
        else
        {
            s_Reads.hero_kit_id = Field<std::string>::Observed(p_Outfit.hero_outfit_id);
            s_Reads.slot0_matches_m5 = Field<bool>::Observed(p_World.playing);
        }

        return s_Reads;
    }

    PlayerObservation::EntityIdentity FakeIdentity(const char* p_EntityId, const char* p_Brick, double p_X, double p_Y, double p_Z, const std::string& p_Mode)
    {
        using OutfitObservation::Field;
        PlayerObservation::EntityIdentity s_Identity;
        const bool s_Predeleted = p_Mode == "invalid-flags";
        s_Identity.resolved = Field<bool>::Observed(true);
        s_Identity.entity_id = Field<std::string>::Observed(p_EntityId);
        s_Identity.entity_flags = Field<int64_t>::Observed(s_Predeleted ? 10 : 7);
        s_Identity.activated = Field<bool>::Observed(!s_Predeleted);
        s_Identity.predeleted = Field<bool>::Observed(s_Predeleted);
        s_Identity.owning_brick = Field<std::string>::Observed(p_Brick);
        s_Identity.character_id = Field<std::string>::Observed(k_FakeCharacterId);
        s_Identity.initial_outfit_id = Field<std::string>::Observed(FakeOutfit::World::k_FakeSuit);
        s_Identity.invincible = Field<bool>::Observed(false);
        s_Identity.x = Field<double>::Observed(p_X);
        s_Identity.y = Field<double>::Observed(p_Y);
        s_Identity.z = Field<double>::Observed(p_Z);
        s_Identity.transform_dirty = Field<bool>::Observed(p_Mode == "dirty-transform");
        s_Identity.room_behaviour = Field<int64_t>::Observed(1);
        return s_Identity;
    }

    PlayerObservation::EntityReads FakeEntity(const FakeWorld& p_World, const std::string& p_Mode, const PlayerObservation::EntityQuery& p_Query)
    {
        using OutfitObservation::Field;
        using CommandProtocol::FieldStatus;

        PlayerObservation::EntityReads s_Reads;
        s_Reads.thread_id = GetCurrentThreadId();

        // the fake brick list: 5 at the menu, 23 in a mission (experiment 4's counts), or scripted
        size_t s_BrickCount = p_World.playing ? 23 : 5;
        if (p_Mode == "bricks-empty") s_BrickCount = 0;
        if (p_Mode == "bricks-oversized") s_BrickCount = 70;
        const bool s_CandidatePresent = p_Mode == "candidate-present";
        const std::string s_Brick = p_Query.brick.value_or(k_FakeHitmenBrick);
        const std::string s_EntityId = p_Query.entity_id.value_or(k_FakeAgent48);

        const size_t s_Total = s_BrickCount + (s_CandidatePresent ? 1 : 0);
        s_Reads.bricks.count = Field<int64_t>::Observed(static_cast<int64_t>(s_Total));
        for (size_t i = 0; i < s_BrickCount && s_Reads.bricks.ids.size() < PlayerObservation::k_MaxBrickIds; ++i)
        {
            char s_Buffer[17] = {};
            std::snprintf(s_Buffer, sizeof(s_Buffer), "00%014zx", i + 1);
            s_Reads.bricks.ids.push_back(i == 1 ? std::string(k_FakeHeroBrick) : std::string(s_Buffer));
        }
        if (s_CandidatePresent && s_Reads.bricks.ids.size() < PlayerObservation::k_MaxBrickIds)
            s_Reads.bricks.ids.push_back(s_Brick);
        s_Reads.bricks.truncated = Field<bool>::Observed(s_Total > PlayerObservation::k_MaxBrickIds);
        s_Reads.bricks.scanned = Field<int64_t>::Observed(static_cast<int64_t>(std::min(s_Total, PlayerObservation::k_MaxBricksScanned))); // the bounded lookup scan (correction C)

        // the local hero: present at the menu and in a mission (experiment 4, F5: a local player
        // exists in the main menu), as the fake registry's slot 0 reports; absent only when scripted
        if (p_Mode == "hero-missing")
        {
            PlayerObservation::MarkIdentity(s_Reads.hero, FieldStatus::Unavailable, "fake hero interface not resolved");
            PlayerObservation::MarkLookup(s_Reads.round_trip, FieldStatus::Unavailable, "fake hero interface not resolved");
        }
        else
        {
            s_Reads.hero = FakeIdentity(k_FakeHeroEntityId, k_FakeHeroBrick, -0.035, -3.832, -0.413, p_Mode);
            auto& s_Trip = s_Reads.round_trip;
            s_Trip.owner_found = Field<bool>::Observed(true);
            s_Trip.factory_available = Field<bool>::Observed(true);
            s_Trip.factory_template = Field<bool>::Observed(p_Mode != "factory-mismatch");
            if (p_Mode == "roundtrip-unrepresentable")
            {
                s_Reads.hero.owning_brick = Field<std::string>::Of(FieldStatus::Unavailable, "fake: no owning entity");
                PlayerObservation::MarkLookup(s_Trip, FieldStatus::Unavailable, "fake: owner not found");
                s_Trip.owner_found = Field<bool>::Observed(false);
            }
            else if (p_Mode == "factory-mismatch")
            {
                s_Trip.subentity_count = Field<int64_t>::Of(FieldStatus::Unavailable, "not a template entity blueprint factory");
                s_Trip.index = Field<int64_t>::Of(FieldStatus::Unavailable, "not a template entity blueprint factory");
                s_Trip.index_check = Field<std::string>::Of(FieldStatus::Unavailable, "not a template entity blueprint factory");
                s_Trip.resolved = Field<bool>::Of(FieldStatus::Unavailable, "not a template entity blueprint factory");
                s_Trip.same_entity = Field<bool>::Of(FieldStatus::Unavailable, "not a template entity blueprint factory");
            }
            else if (p_Mode == "subentity-missing")
            {
                s_Trip.subentity_count = Field<int64_t>::Observed(2);
                s_Trip.index = Field<int64_t>::Observed(-1);
                s_Trip.index_check = Field<std::string>::Observed(PlayerObservation::IndexCheckText(PlayerObservation::IndexCheck::Negative));
                s_Trip.resolved = Field<bool>::Of(FieldStatus::Unavailable, "not a sub-entity of that factory");
                s_Trip.same_entity = Field<bool>::Of(FieldStatus::Unavailable, "not a sub-entity of that factory");
            }
            else
            {
                s_Trip.subentity_count = Field<int64_t>::Observed(2);
                s_Trip.index = Field<int64_t>::Observed(1);
                s_Trip.index_check = Field<std::string>::Observed(PlayerObservation::IndexCheckText(PlayerObservation::IndexCheck::InRange));
                s_Trip.resolved = Field<bool>::Observed(true);
                s_Trip.same_entity = Field<bool>::Observed(p_Mode != "roundtrip-different");
            }
        }
        s_Reads.round_trip_outcome = PlayerObservation::ClassifyRoundTrip(s_Reads.round_trip);

        // the candidate
        PlayerObservation::CandidateReads s_Candidate;
        s_Candidate.brick_id = s_Brick;
        s_Candidate.entity_id = s_EntityId;
        if (s_CandidatePresent)
        {
            // the scripted candidate lies inside the scanned prefix
            s_Candidate.brick_present = PlayerObservation::CandidatePresence(true, s_Reads.bricks);
            s_Candidate.lookup.owner_found = Field<bool>::Observed(true);
            s_Candidate.lookup.factory_available = Field<bool>::Observed(true);
            s_Candidate.lookup.factory_template = Field<bool>::Observed(true);
            s_Candidate.lookup.subentity_count = Field<int64_t>::Observed(3);
            s_Candidate.lookup.index = Field<int64_t>::Observed(0);
            s_Candidate.lookup.index_check = Field<std::string>::Observed(PlayerObservation::IndexCheckText(PlayerObservation::IndexCheck::InRange));
            s_Candidate.lookup.resolved = Field<bool>::Observed(true);
            s_Candidate.lookup.same_entity = Field<bool>::Observed(false);
            s_Candidate.identity = FakeIdentity(s_EntityId.c_str(), s_Brick.c_str(), -40.99882, -24.293657, 0.4925565, p_Mode);
            s_Candidate.hitman5_resolved = Field<bool>::Observed(true);
        }
        else
        {
            // absent from the scanned prefix: "false" only from a complete scan, otherwise unavailable
            // with the scan bound as the reason (correction C), decided by the same rule the DLL applies
            s_Candidate.brick_present = PlayerObservation::CandidatePresence(false, s_Reads.bricks);
            const std::string s_Why = s_Candidate.brick_present.observed() ? "candidate brick not loaded" : s_Candidate.brick_present.reason;
            PlayerObservation::MarkLookup(s_Candidate.lookup, FieldStatus::Unavailable, s_Why.c_str());
            PlayerObservation::MarkIdentity(s_Candidate.identity, FieldStatus::Unavailable, s_Why.c_str());
            s_Candidate.hitman5_resolved = Field<bool>::Of(FieldStatus::Unavailable, s_Why);
        }
        s_Reads.candidate = s_Candidate;
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

        bool exit_after_call = false; // M5 C-2C: terminate the process right after the fake call returned

        OutfitApply::CallOutcome Invoke(int32_t p_Charset, int32_t p_Variation) override
        {
            const auto s_Outcome = inner.Invoke(p_Charset, p_Variation);
            RelayLog::Info("command probe: FAKE setter invoked (charset {}, variation {}); called {} faulted {} — simulated, no engine", p_Charset, p_Variation, s_Outcome.called, s_Outcome.faulted);
            std::printf("command probe: FAKE setter invoked (charset %d, variation %d); called %d faulted %d (simulated; no engine)\n", p_Charset, p_Variation, s_Outcome.called ? 1 : 0, s_Outcome.faulted ? 1 : 0);

            if (exit_after_call)
            {
                // Link loss after application and before result delivery: the process ends here, on
                // the fake frame thread, with the fake setter's effect in place and no result ever
                // written to the link (the probe's clean shutdown is deliberately not run).
                RelayLog::Info("command probe: terminating the process right after the FAKE setter returned (simulated link loss after application, before the result)");
                std::printf("command probe: terminating the process right after the FAKE setter returned (simulated link loss after application, before the result)\n");
                std::fflush(stdout);
                TerminateProcess(GetCurrentProcess(), 3);
            }

            return s_Outcome;
        }

        OutfitObservation::EngineReads ReadBack() override { return inner.ReadBack(); }
        bool Latched() const override { return inner.Latched(); }
        void Release() override { inner.Release(); }
    };

    // M5 C-2C: the scripted raw log writer for the "log-*" apply modes. Installed through
    // RelayLog::SetRawWriterForTests (the test seam of correction B; never set in the DLL), so the
    // production WriteLine / WriteChecked / OutfitApply path runs unchanged and only the lowest
    // write is scripted. The FIRST line containing the apply's "applying" marker is reported as not
    // accepted in the scripted way; every other line — and every later applying line — is passed
    // through to a probe-owned file (the real per-process log file stays open but unused while the
    // seam is installed, which the probe states in its output).
    enum class LogMode { None, Unavailable, Failed, Short };
    LogMode g_LogMode = LogMode::None;
    int g_ApplyingLinesSeen = 0;
    HANDLE g_PassThrough = INVALID_HANDLE_VALUE;

    RelayLog::RawWrite PassThrough(const char* p_Data, uint32_t p_Size)
    {
        RelayLog::RawWrite s_Raw;
        if (g_PassThrough == INVALID_HANDLE_VALUE)
            return s_Raw; // file_open false: the probe could not open its pass-through file
        s_Raw.file_open = true;
        DWORD s_Written = 0;
        s_Raw.succeeded = WriteFile(g_PassThrough, p_Data, p_Size, &s_Written, nullptr) != FALSE;
        s_Raw.written = s_Written;
        s_Raw.error = s_Raw.succeeded ? 0 : GetLastError();
        return s_Raw;
    }

    RelayLog::RawWrite ScriptedRawWrite(const char* p_Data, uint32_t p_Size)
    {
        const bool s_Applying = std::string_view(p_Data, p_Size).find("applying player.outfit.set/2") != std::string_view::npos;
        if (!s_Applying || g_ApplyingLinesSeen++ != 0 || g_LogMode == LogMode::None)
        {
            if (s_Applying)
                std::printf("command probe: FAKE log writer: applying line %d passed through and accepted (%u bytes)\n", g_ApplyingLinesSeen, p_Size);
            return PassThrough(p_Data, p_Size);
        }

        RelayLog::RawWrite s_Raw;
        const char* s_What = "";
        switch (g_LogMode)
        {
            case LogMode::Unavailable:
                s_Raw.file_open = false;
                s_Raw.error = ERROR_PATH_NOT_FOUND;
                s_What = "file not open (simulated ERROR_PATH_NOT_FOUND)";
                break;
            case LogMode::Failed:
                s_Raw.file_open = true;
                s_Raw.succeeded = false;
                s_Raw.error = ERROR_WRITE_FAULT;
                s_What = "WriteFile failed (simulated ERROR_WRITE_FAULT)";
                break;
            case LogMode::Short:
                s_Raw.file_open = true;
                s_Raw.succeeded = true;
                s_Raw.written = p_Size > 7 ? p_Size - 7 : 0;
                s_What = "short write (simulated: 7 bytes fewer than requested)";
                break;
            default:
                break;
        }
        std::printf("command probe: FAKE log writer: applying line 1 NOT accepted — %s; %u bytes requested, %u accepted (simulated; the line is not in any file)\n", s_What, p_Size, s_Raw.written);
        return s_Raw;
    }

    bool OpenPassThrough()
    {
        wchar_t s_Dir[MAX_PATH] = {};
        DWORD s_Len = GetEnvironmentVariableW(L"GLACIERRELAY_LOG_DIR", s_Dir, MAX_PATH);
        if (s_Len == 0 || s_Len >= MAX_PATH)
        {
            s_Len = GetTempPathW(MAX_PATH, s_Dir);
            if (s_Len == 0 || s_Len >= MAX_PATH)
                return false;
        }
        wchar_t s_Path[MAX_PATH] = {};
        swprintf_s(s_Path, L"%s\\command-probe-passthrough-%lu.log", s_Dir, GetCurrentProcessId());
        g_PassThrough = CreateFileW(s_Path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        return g_PassThrough != INVALID_HANDLE_VALUE;
    }
}

int main(int p_Argc, char** p_Argv)
{
    if (p_Argc < 4)
    {
        std::printf("usage: GlacierRelayCommandProbe <port> <frame_interval_ms> <duration_ms> [menu|mission|unavailable|restart|no-contract-start|contract-ended|registry-ahead] [inert|effectful] [ok|unchanged|elsewhere|fault|readback-missing|post-generation|setter-unavailable|exit-after-call|log-unavailable|log-failed|log-short] [default|registry-unavailable|layout-unsupported|extra-slot|hero-missing|candidate-present|roundtrip-different|roundtrip-unrepresentable|factory-mismatch|subentity-missing|bricks-empty|bricks-oversized|dirty-transform|invalid-flags]\n");
        return 2;
    }

    const uint16_t s_Port = static_cast<uint16_t>(std::atoi(p_Argv[1]));
    const uint32_t s_Interval = static_cast<uint32_t>(std::atoi(p_Argv[2]));
    const uint32_t s_Duration = static_cast<uint32_t>(std::atoi(p_Argv[3]));
    const std::string s_SceneMode = p_Argc > 4 ? p_Argv[4] : "menu";
    const bool s_Effectful = p_Argc > 5 && std::string(p_Argv[5]) == "effectful";
    const std::string s_ApplyMode = p_Argc > 6 ? p_Argv[6] : "ok";
    const std::string s_PlayersMode = p_Argc > 7 ? p_Argv[7] : "default"; // M6-B1

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
    RelayLog::Info("command probe: adapter instance {}, port {}, frame interval {} ms, duration {} ms, scene {}, {} , apply-mode {}, players-mode {}, token {}, main thread {} (FAKE frame source, FAKE inspection data, FAKE registry/entity data and FAKE applier; never HITMAN state)", s_Adapter, s_Port, s_Interval, s_Duration, s_SceneMode, s_Effectful ? "effectful" : "inert", s_ApplyMode, s_PlayersMode, s_Token.empty() ? "absent (protocol version 1)" : "present (protocol version 2; not logged)", GetCurrentThreadId());
    std::printf("command probe: players-mode %s (FAKE registry, brick, entity and transform data; never HITMAN state)\n", s_PlayersMode.c_str());
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
    else if (s_ApplyMode == "exit-after-call") s_Applier.exit_after_call = true;
    else if (s_ApplyMode == "log-unavailable") g_LogMode = LogMode::Unavailable;
    else if (s_ApplyMode == "log-failed") g_LogMode = LogMode::Failed;
    else if (s_ApplyMode == "log-short") g_LogMode = LogMode::Short;
    const bool s_SetterUnavailable = s_ApplyMode == "setter-unavailable";

    if (g_LogMode != LogMode::None)
    {
        const bool s_Opened = OpenPassThrough();
        const auto s_Previous = RelayLog::SetRawWriterForTests(&ScriptedRawWrite);
        std::printf("command probe: scripted raw log writer installed through the test seam (previous writer %s; pass-through file %s): the first applying line will be reported as not accepted, later lines pass through; the per-process relay log file is not written while the seam is installed\n", s_Previous ? "set" : "null", s_Opened ? "open" : "NOT open");
        RelayLog::Info("command probe: scripted raw log writer installed (mode {}); this line and the following ones are in the pass-through file", s_ApplyMode);
    }

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
        const bool s_Mission = s_SceneMode == "mission" || s_SceneMode == "restart" || s_SceneMode == "no-contract-start" || s_SceneMode == "contract-ended" || s_SceneMode == "registry-ahead";
        FakeWorld s_World;
        s_World.setter_available = !s_SetterUnavailable;
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
            if (s_SceneMode == "registry-ahead" && s_Frame >= 6)
                s_Registry = s_Generation2; // the registry moved on; the predicate and the cached generation did not
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
                if (s_SceneMode != "no-contract-start")
                    s_ContractStarted = s_Registry; // contract.started observed right after the rise
            }
            else if (!s_Playing && s_PrevPlaying)
            {
                s_Advance("mission predicate fall");
                s_Cached.reset();
            }
            if ((s_SceneMode == "restart" && s_Frame == 8) || (s_SceneMode == "contract-ended" && s_Frame == 6))
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
            // M6-B1: the FAKE registry and entity readers (scripted per players-mode; never HITMAN state)
            s_Observation.players_inspector = [&] { return FakePlayers(s_World, s_Applier.inner.world, s_PlayersMode); };
            s_Observation.entity_inspector = [&](const PlayerObservation::EntityQuery& p_Query) { return FakeEntity(s_World, s_PlayersMode, p_Query); };
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
        "command probe: link version %u effectful %d connects %llu disconnects %llu refusals %llu attests %llu generation %llu; lines %llu queued %llu busy %llu duplicate %llu rejected %llu violations %llu results_sent %llu cleared %llu; reconnect waits %llu backoff now %u ms stable links %llu; network thread %lu\n",
        s_Stats.protocol_version, s_Stats.effectful ? 1 : 0, s_Stats.connects, s_Stats.disconnects, s_Stats.refusals, s_Stats.attests_sent, s_Stats.link_generation, s_Stats.lines_received, s_Stats.commands_queued, s_Stats.commands_busy, s_Stats.commands_duplicate, s_Stats.commands_rejected, s_Stats.protocol_violations, s_Stats.results_sent, s_Stats.cleared_on_disconnect, s_Stats.reconnect_waits, s_Stats.current_backoff_ms, s_Stats.stable_links, s_Stats.network_thread_id
    );
    std::printf(
        "command probe: queue pushed %llu popped %llu busy %llu duplicates %llu cleared %llu remembered %zu; scheduler frames %llu executed %llu inspected %llu players_inspected %llu entities_inspected %llu outfit dispatched %llu called %llu changed %llu not_applied %llu uncertain %llu reduced %llu expired %llu stale %llu rejected %llu first thread %lu last thread %lu changed %d last latency %llu ms max %llu ms; FAKE applier observes %d invokes %d readbacks %d latched %d\n",
        s_Queued.pushed, s_Queued.popped, s_Queued.busy, s_Queued.duplicates, s_Queued.cleared, s_Queued.remembered, s_Scheduled.frames, s_Scheduled.executed, s_Scheduled.inspected, s_Scheduled.players_inspected, s_Scheduled.entities_inspected, s_Scheduled.outfit_dispatched, s_Scheduled.outfit_called, s_Scheduled.outfit_changed, s_Scheduled.outfit_not_applied, s_Scheduled.outfit_uncertain, s_Scheduled.reduced, s_Scheduled.expired, s_Scheduled.stale_generation, s_Scheduled.rejected, s_Scheduled.first_thread_id, s_Scheduled.last_thread_id, s_Scheduled.thread_changed ? 1 : 0, s_Scheduled.last_latency_ms, s_Scheduled.max_latency_ms,
        s_Applier.inner.observes, s_Applier.inner.invokes, s_Applier.inner.readbacks, s_Applier.inner.Latched() ? 1 : 0
    );
    RelayLog::Info("command probe: done after {} fake frames; connects {}, executed {}, inspected {}, players inspected {}, entities inspected {}, outfit dispatched {} called {} changed {}, expired {}, violations {}", s_Frames, s_Stats.connects, s_Scheduled.executed, s_Scheduled.inspected, s_Scheduled.players_inspected, s_Scheduled.entities_inspected, s_Scheduled.outfit_dispatched, s_Scheduled.outfit_called, s_Scheduled.outfit_changed, s_Scheduled.expired, s_Stats.protocol_violations);

    return s_Stats.connects > 0 ? 0 : 1;
}
