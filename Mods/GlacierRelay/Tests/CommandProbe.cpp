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
// The M6-B1 hardening stage adds the modes its three corrections need, every one a scripted FAKE
// factory or list answer: "index-at-count" (count 2, index 2), "index-beyond-count" (count 2,
// index 5), "count-negative" (count -1: the index is not looked up), "count-zero" (count 0, index
// -1); "bricks-at-bound" (64 bricks, the candidate absent: a complete scan), "bricks-oversized"
// now reports the candidate's absence as not established (70 bricks, 64 scanned),
// "bricks-oversized-candidate" (70 + the candidate, scripted as found inside the scanned prefix);
// "texts-maximal" (every text field 128 bytes, every other field unavailable with a 40-byte
// reason: the first reduction stage), "texts-maximal-candidate" (the same with the candidate
// present: the result at the 48-entry cap), "texts-escaped" (texts with quotes, backslashes, a
// tab, a control character and a two-byte UTF-8 character), "reasons-maximal" (every registry and
// entity field unavailable with a 40-byte reason). Pathological by design; never HITMAN state.
// candidate-mode (M6-D1a): the FAKE candidate, spawn and registry-identity state the read-only
// candidate inspection reports (relay.inspect.candidate); the modes are documented in
// FakeCandidate.h ("default" = the candidate brick absent). Every value is scripted and is never an
// observation of HITMAN.
// transform-mode (M6-F): the FAKE exact-transform world the inspection (relay.inspect.candidate_transform)
// and the transform apply (candidate.transform.set) read: "present" (default: the candidate resolved,
// distinct, in no slot, clean, at the M6-D1b shape; absent at the menu), "absent", "local-hero-alias",
// "wrong-brick-owner", "index-invalid", "in-registry", "inconclusive-slot", "dirty-preimage",
// "no-spatial", "no-hitman5", "hero-unavailable", "registry-unavailable", "twin-bits" (the candidate's
// z carries the seven-digit twin of the M6-D1b value: the expectation must differ bit for bit),
// "at-p" (M6-G1: the candidate's and the local hero's exact words as M6-E3's r3 read them — the
// candidate at P, c2b2fae1 416d47ae bfc2d6ba with identity axes and the signed-zero parent-relative
// quaternion 80000000 00000000 80000000 3f800000; scripted copies, never a live read).
// transform-apply-mode (M6-F, effectful only): "ok" (the fake setter stores the twelve requested
// words), "unchanged", "elsewhere", "axes-elsewhere", "local-follows", "dirty-post", "readback-missing",
// "post-generation", "post-parent", "post-entity", "fault", "control-changes", "control-drifts",
// "exit-after-call" (as the outfit mode of that name); the log-* apply modes above also script the
// FIRST "applying candidate.transform.set/<1|2>" line. transform-later (M6-F): what the first inspection
// AFTER a fake forward (a v1 lift or a v2 step_y) shows — "held" (default), "returned", "near", "drifted", "dirty". Every value is
// scripted and is never an observation of HITMAN; no engine, no SDK function.
// M7-B2 adds the motion mode, from the environment (no new positional argument): with
// GLACIER_PROBE_MOTION_PORT set (with the token: the motion link binds only to an attested version-2
// command link) the probe runs the real MotionLink to BEAM's motion listener (127.0.0.1:<port>), the
// real Motion::Controller on its fake frame thread after the scheduler, and advertises
// player.pose.share/1 (and candidate.motion.session/1 when effectful) — exactly the DLL's [motion]
// wiring, with the FAKE transform world as the engine: the sampler reads the FAKE local hero, the
// session's one setter call is the FAKE applier's. Every motion line the native queued is printed
// ("command probe: motion> <line>", with its queue outcome) so a driver can compare it with BEAM's
// trace value for value. GLACIER_PROBE_MOTION_SCRIPT (comma-separated, scripted FAKE events):
// hero-step-mm:<n> (the FAKE hero's x grows by n mm per frame), inexact-from:<k> (from the k-th FAKE
// setter call on, the FAKE setter leaves z off the request: the first inexact postimage),
// stall-at:<frame>:<ms> (the fake frame thread stops for ms before that frame: a stalled frame
// callback), z-drift-at:<frame> (the FAKE engine changes the candidate's z word at that frame: an
// untouched-word change), hero-dirty-every:<k> (every k-th frame the FAKE hero reads dirty),
// stall-at-invoke:<k>:<ms> and z-drift-at-invoke:<k> (the same two events, once, on the first frame
// after the k-th FAKE setter call: an end-to-end run cannot know frame numbers in advance).
// GLACIER_PROBE_STDIN_EXIT=1 ends the probe when its standard input closes (bounded teardown by its
// owner); the duration argument stays the failsafe. Every value is scripted; nothing is HITMAN state.
// Exit code 0 when the link connected and was welcomed at least once.

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <optional>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>

#include "CommandLink.h"
#include "CommandQueue.h"
#include "CommandScheduler.h"
#include "FakeCandidate.h"
#include "FakeOutfitApplier.h"
#include "FakeTransformApplier.h"
#include "MotionController.h"
#include "MotionLink.h"
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

    // M6-B1 hardening: the maximal reason (40 bytes) and the maximal text (128 bytes) the pathological
    // modes script, and the escaped texts the texts-escaped mode scripts (the driver expects the same
    // bytes back after BEAM's JSON decoding).
    const std::string k_MaxReason(CommandProtocol::k_MaxObservedReasonBytes, 'r');
    std::string MaxText(char p_Fill) { return std::string(CommandProtocol::k_MaxObservedTextBytes, p_Fill); }
    constexpr const char* k_EscapedOwning = "\"back\\slash\ttab\x01" "ctl\xC3\xA9" "acute/end";
    const std::string k_EscapedCharacter(64, '"');
    const std::string k_EscapedInitial(42, '\x01');

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

        if (p_Mode == "reasons-maximal")
        {
            // every registry field unavailable with a maximal reason: the line exceeds the bound with
            // the reasons and fits without them (the first reduction stage, every field kept)
            auto s_Reads = PlayerObservation::AllRegistry(FieldStatus::Unavailable, k_MaxReason.c_str());
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

        if (p_Mode == "reasons-maximal")
        {
            // every entity field unavailable with a maximal reason (the candidate echoed); this shape
            // fits the line with its reasons, which the driver verifies
            auto s_All = PlayerObservation::AllEntity(FieldStatus::Unavailable, k_MaxReason.c_str(), p_Query);
            s_All.candidate->brick_id = p_Query.brick.value_or(k_FakeHitmenBrick);
            s_All.candidate->entity_id = p_Query.entity_id.value_or(k_FakeAgent48);
            s_All.thread_id = GetCurrentThreadId();
            return s_All;
        }

        // the fake brick list: 5 at the menu, 23 in a mission (experiment 4's counts), or scripted
        size_t s_BrickCount = p_World.playing ? 23 : 5;
        if (p_Mode == "bricks-empty") s_BrickCount = 0;
        if (p_Mode == "bricks-oversized" || p_Mode == "bricks-oversized-candidate") s_BrickCount = 70;
        if (p_Mode == "bricks-at-bound") s_BrickCount = 64;
        const bool s_CandidatePresent = p_Mode == "candidate-present" || p_Mode == "bricks-oversized-candidate" || p_Mode == "texts-maximal-candidate";
        const bool s_TextsMaximal = p_Mode == "texts-maximal" || p_Mode == "texts-maximal-candidate";
        const bool s_TextsEscaped = p_Mode == "texts-escaped";
        const std::string s_Brick = p_Query.brick.value_or(k_FakeHitmenBrick);
        const std::string s_EntityId = p_Query.entity_id.value_or(k_FakeAgent48);

        const size_t s_Total = s_BrickCount + (s_CandidatePresent ? 1 : 0);
        s_Reads.bricks.count = Field<int64_t>::Observed(static_cast<int64_t>(s_Total));
        for (size_t i = 0; i < s_BrickCount && s_Reads.bricks.ids.size() < PlayerObservation::k_MaxBrickIds; ++i)
        {
            char s_Buffer[17] = {};
            std::snprintf(s_Buffer, sizeof(s_Buffer), "00%014zx", i + 1);
            // texts-maximal: FAKE 128-byte "ids" so each id field reaches the text bound (the encoder truncates the joined text)
            s_Reads.bricks.ids.push_back(s_TextsMaximal ? MaxText('a' + static_cast<char>(i % 26)) : (i == 1 ? std::string(k_FakeHeroBrick) : std::string(s_Buffer)));
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
            if (s_TextsMaximal)
            {
                // every text at the bound; every non-text field unavailable with a maximal reason
                s_Reads.hero.entity_id = Field<std::string>::Observed(MaxText('e'));
                s_Reads.hero.owning_brick = Field<std::string>::Observed(MaxText('o'));
                s_Reads.hero.character_id = Field<std::string>::Observed(MaxText('c'));
                s_Reads.hero.initial_outfit_id = Field<std::string>::Observed(MaxText('i'));
                s_Reads.hero.entity_flags = Field<int64_t>::Of(FieldStatus::Unavailable, k_MaxReason);
                s_Reads.hero.activated = Field<bool>::Of(FieldStatus::Unavailable, k_MaxReason);
                s_Reads.hero.predeleted = Field<bool>::Of(FieldStatus::Unavailable, k_MaxReason);
                s_Reads.hero.invincible = Field<bool>::Of(FieldStatus::Unavailable, k_MaxReason);
                s_Reads.hero.x = Field<double>::Of(FieldStatus::Unavailable, k_MaxReason);
                s_Reads.hero.y = Field<double>::Of(FieldStatus::Unavailable, k_MaxReason);
                s_Reads.hero.z = Field<double>::Of(FieldStatus::Unavailable, k_MaxReason);
                s_Reads.hero.transform_dirty = Field<bool>::Of(FieldStatus::Unavailable, k_MaxReason);
                s_Reads.hero.room_behaviour = Field<int64_t>::Of(FieldStatus::Unavailable, k_MaxReason);
            }
            if (s_TextsEscaped)
            {
                s_Reads.hero.owning_brick = Field<std::string>::Observed(k_EscapedOwning);
                s_Reads.hero.character_id = Field<std::string>::Observed(k_EscapedCharacter);
                s_Reads.hero.initial_outfit_id = Field<std::string>::Observed(k_EscapedInitial);
            }
            auto& s_Trip = s_Reads.round_trip;
            s_Trip.owner_found = Field<bool>::Observed(true);
            s_Trip.factory_available = Field<bool>::Observed(true);
            s_Trip.factory_template = Field<bool>::Observed(p_Mode != "factory-mismatch");
            if (s_TextsMaximal)
            {
                // the lookup facts unavailable with maximal reasons too (the check as a text at its bound)
                PlayerObservation::MarkLookup(s_Trip, FieldStatus::Unavailable, k_MaxReason.c_str());
                s_Trip.owner_found = Field<bool>::Observed(true);
                s_Trip.index_check = Field<std::string>::Observed(MaxText('k'));
            }
            else if (p_Mode == "index-at-count" || p_Mode == "index-beyond-count")
            {
                // correction A: a non-negative index at or beyond the count is an inconsistent factory answer
                s_Trip.subentity_count = Field<int64_t>::Observed(2);
                s_Trip.index = Field<int64_t>::Observed(p_Mode == "index-at-count" ? 2 : 5);
                s_Trip.index_check = Field<std::string>::Observed(PlayerObservation::IndexCheckText(PlayerObservation::CheckSubEntityIndex(2, p_Mode == "index-at-count" ? 2 : 5)));
                s_Trip.resolved = Field<bool>::Of(FieldStatus::Inconsistent, "index outside the sub-entity count");
                s_Trip.same_entity = Field<bool>::Of(FieldStatus::Inconsistent, "index outside the sub-entity count");
            }
            else if (p_Mode == "count-negative")
            {
                s_Trip.subentity_count = Field<int64_t>::Observed(-1);
                s_Trip.index = Field<int64_t>::Of(FieldStatus::Unavailable, "count invalid; index not looked up");
                s_Trip.index_check = Field<std::string>::Observed(PlayerObservation::IndexCheckText(PlayerObservation::IndexCheck::CountInvalid));
                s_Trip.resolved = Field<bool>::Of(FieldStatus::Inconsistent, "sub-entity count invalid");
                s_Trip.same_entity = Field<bool>::Of(FieldStatus::Inconsistent, "sub-entity count invalid");
            }
            else if (p_Mode == "count-zero")
            {
                s_Trip.subentity_count = Field<int64_t>::Observed(0);
                s_Trip.index = Field<int64_t>::Observed(-1);
                s_Trip.index_check = Field<std::string>::Observed(PlayerObservation::IndexCheckText(PlayerObservation::CheckSubEntityIndex(0, -1)));
                s_Trip.resolved = Field<bool>::Of(FieldStatus::Unavailable, "not a sub-entity of that factory");
                s_Trip.same_entity = Field<bool>::Of(FieldStatus::Unavailable, "not a sub-entity of that factory");
            }
            else if (p_Mode == "roundtrip-unrepresentable")
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
            if (s_TextsMaximal)
            {
                s_Candidate.identity.character_id = Field<std::string>::Observed(MaxText('k'));
                s_Candidate.identity.entity_flags = Field<int64_t>::Of(FieldStatus::Unavailable, k_MaxReason);
                s_Candidate.identity.x = Field<double>::Of(FieldStatus::Unavailable, k_MaxReason);
                s_Candidate.identity.y = Field<double>::Of(FieldStatus::Unavailable, k_MaxReason);
                s_Candidate.identity.z = Field<double>::Of(FieldStatus::Unavailable, k_MaxReason);
                s_Candidate.identity.transform_dirty = Field<bool>::Of(FieldStatus::Unavailable, k_MaxReason);
                s_Candidate.lookup.index_check = Field<std::string>::Observed(MaxText('q'));
            }
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

    // M6-F: the probe's FAKE transform applier shares one scripted transform world with the probe's
    // exact-transform inspector, so an inspection after a fake lift reports the words the fake setter
    // left (and the scripted later frame once). Never HITMAN state; no engine pointer exists here.
    class ProbeTransformApplier final : public TransformApply::ITransformApplier
    {
    public:
        FakeTransform::Applier inner;     // the scripted state machine the tests use
        const FakeWorld* world = nullptr; // the frame's scripted scene (generation, hero presence)
        bool exit_after_call = false;

        void SyncFrame()
        {
            // the local hero exists at the menu too (experiment 4, F5; the other FAKE inspectors script it so): only the
            // scripted hero-unavailable mode removes it
            inner.world.hero_readable = world && !hero_unavailable;
            inner.world.fresh_generation = world ? world->registry_generation : "";
            inner.world.fresh_unavailable = !world || world->registry_generation.empty();
            if (inner.world.mode != FakeTransform::Mode::PostGenerationChange || inner.invokes == 0)
                inner.world.post_generation = inner.world.fresh_generation;
            // the candidate exists only in a mission world (the content is Paris-scoped)
            inner.world.brick_present = world && world->playing && !absent;
        }

        bool absent = false;
        bool hero_unavailable = false;

        TransformObservation::Reads Observe() override
        {
            SyncFrame();
            return inner.Observe();
        }

        TransformApply::CallOutcome Invoke(const FloatWords::Words12& p_World) override
        {
            const auto s_Outcome = inner.Invoke(p_World);
            RelayLog::Info("command probe: FAKE transform setter invoked (world {}); called {} faulted {} — simulated, no engine", FloatWords::Encode(p_World), s_Outcome.called, s_Outcome.faulted);
            std::printf("command probe: FAKE transform setter invoked (world %s); called %d faulted %d (simulated; no engine)\n", FloatWords::Encode(p_World).c_str(), s_Outcome.called ? 1 : 0, s_Outcome.faulted ? 1 : 0);

            if (exit_after_call)
            {
                RelayLog::Info("command probe: terminating the process right after the FAKE transform setter returned (simulated link loss after application, before the result)");
                std::printf("command probe: terminating the process right after the FAKE transform setter returned (simulated link loss after application, before the result)\n");
                std::fflush(stdout);
                TerminateProcess(GetCurrentProcess(), 3);
            }

            return s_Outcome;
        }

        TransformObservation::Reads ReadBack() override { return inner.ReadBack(); }
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
        const std::string_view s_Line(p_Data, p_Size);
        const bool s_Applying = s_Line.find("applying player.outfit.set/2") != std::string_view::npos || s_Line.find("applying candidate.transform.set/") != std::string_view::npos; // M6-G1: /1 and /2
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

namespace
{
    // M7-B2: the probe's view of the real motion link, printing every line the native queued (the
    // queue outcome beside it) for the driver's value-for-value comparison; everything else is the
    // real link's.
    class RecordingLink final : public IMotionLink
    {
    public:
        explicit RecordingLink(MotionLink& p_Link) : m_Link(p_Link) {}
        Binding Current() const override { return m_Link.Current(); }
        std::vector<Inbound> Drain() override { return m_Link.Drain(); }
        uint64_t Signals() const override { return m_Link.Signals(); }
        Queued SendControl(uint64_t p_Serial, std::string p_Line) override { return Record("control", m_Link.SendControl(p_Serial, p_Line), p_Line); }
        Queued SendSample(uint64_t p_Serial, std::string p_Line) override { return Record("sample", m_Link.SendSample(p_Serial, p_Line), p_Line); }
        void Drop(uint64_t p_Serial, const char* p_Reason) override
        {
            std::printf("command probe: motion drop serial %llu (%s)\n", static_cast<unsigned long long>(p_Serial), p_Reason);
            m_Link.Drop(p_Serial, p_Reason);
        }

    private:
        Queued Record(const char* p_Queue, Queued p_Outcome, const std::string& p_Line)
        {
            const char* s_Outcome = p_Outcome == Queued::Queued ? "queued" : p_Outcome == Queued::Full ? "full" : "not_bound";
            std::string s_Line = p_Line;
            if (!s_Line.empty() && s_Line.back() == '\n')
                s_Line.pop_back();
            std::printf("command probe: motion> %s %s %s\n", p_Queue, s_Outcome, s_Line.c_str());
            return p_Outcome;
        }

        MotionLink& m_Link;
    };

    struct MotionScript
    {
        uint32_t hero_step_mm = 0;
        uint32_t inexact_from = 0;
        uint64_t stall_frame = 0;
        uint32_t stall_ms = 0;
        uint64_t z_drift_frame = 0;
        uint32_t hero_dirty_every = 0;
        uint32_t stall_after_invokes = 0;
        uint32_t z_drift_after_invokes = 0;
    };

    MotionScript ParseMotionScript(const std::string& p_Text)
    {
        MotionScript s_Script;
        size_t s_At = 0;
        while (s_At < p_Text.size())
        {
            size_t s_End = p_Text.find(',', s_At);
            if (s_End == std::string::npos)
                s_End = p_Text.size();
            const std::string s_Item = p_Text.substr(s_At, s_End - s_At);
            s_At = s_End + 1;
            const size_t s_Colon = s_Item.find(':');
            const std::string s_Key = s_Item.substr(0, s_Colon);
            const std::string s_Value = s_Colon == std::string::npos ? "" : s_Item.substr(s_Colon + 1);
            if (s_Key == "hero-step-mm") s_Script.hero_step_mm = static_cast<uint32_t>(std::atoi(s_Value.c_str()));
            else if (s_Key == "inexact-from") s_Script.inexact_from = static_cast<uint32_t>(std::atoi(s_Value.c_str()));
            else if (s_Key == "z-drift-at") s_Script.z_drift_frame = static_cast<uint64_t>(std::atoll(s_Value.c_str()));
            else if (s_Key == "hero-dirty-every") s_Script.hero_dirty_every = static_cast<uint32_t>(std::atoi(s_Value.c_str()));
            else if (s_Key == "z-drift-at-invoke") s_Script.z_drift_after_invokes = static_cast<uint32_t>(std::atoi(s_Value.c_str()));
            else if (s_Key == "stall-at-invoke")
            {
                const size_t s_Second = s_Value.find(':');
                s_Script.stall_after_invokes = static_cast<uint32_t>(std::atoi(s_Value.substr(0, s_Second).c_str()));
                s_Script.stall_ms = s_Second == std::string::npos ? 0 : static_cast<uint32_t>(std::atoi(s_Value.substr(s_Second + 1).c_str()));
            }
            else if (s_Key == "stall-at")
            {
                const size_t s_Second = s_Value.find(':');
                s_Script.stall_frame = static_cast<uint64_t>(std::atoll(s_Value.substr(0, s_Second).c_str()));
                s_Script.stall_ms = s_Second == std::string::npos ? 0 : static_cast<uint32_t>(std::atoi(s_Value.substr(s_Second + 1).c_str()));
            }
            else if (!s_Key.empty())
                std::printf("command probe: motion script item '%s' unknown (ignored)\n", s_Item.c_str());
        }
        return s_Script;
    }

    std::string Env(const char* p_Name)
    {
        char s_Buffer[512] = {};
        const DWORD s_Len = GetEnvironmentVariableA(p_Name, s_Buffer, sizeof(s_Buffer));
        return s_Len > 0 && s_Len < sizeof(s_Buffer) ? std::string(s_Buffer, s_Len) : std::string();
    }
}

int main(int p_Argc, char** p_Argv)
{
    if (p_Argc < 4)
    {
        std::printf("usage: GlacierRelayCommandProbe <port> <frame_interval_ms> <duration_ms> [menu|mission|unavailable|restart|no-contract-start|contract-ended|registry-ahead] [inert|effectful] [ok|unchanged|elsewhere|fault|readback-missing|post-generation|setter-unavailable|exit-after-call|log-unavailable|log-failed|log-short] [default|registry-unavailable|layout-unsupported|extra-slot|hero-missing|candidate-present|roundtrip-different|roundtrip-unrepresentable|factory-mismatch|subentity-missing|bricks-empty|bricks-oversized|dirty-transform|invalid-flags|index-at-count|index-beyond-count|count-negative|count-zero|bricks-at-bound|bricks-oversized-candidate|texts-maximal|texts-maximal-candidate|texts-escaped|reasons-maximal] [candidate-mode: default|candidate-present|candidate-incomplete|candidate-in-slot|candidate-in-slot-nohero|spawn-other|spawn-missing|entity-missing|hero-interface-missing|parent-none|parent-unavailable|dirty-transform|hidden|active-unavailable|active-getter|registry-unavailable|count-invalid|index-at-count|bricks-oversized|texts-maximal|reasons-maximal|scene-unavailable] [transform-mode: present|absent|local-hero-alias|wrong-brick-owner|index-invalid|in-registry|inconclusive-slot|dirty-preimage|no-spatial|no-hitman5|hero-unavailable|registry-unavailable|twin-bits|at-p] [transform-apply-mode: ok|unchanged|elsewhere|axes-elsewhere|local-follows|dirty-post|readback-missing|post-generation|post-parent|post-entity|fault|control-changes|control-drifts|exit-after-call] [transform-later: held|returned|near|drifted|dirty]\n");
        return 2;
    }

    const uint16_t s_Port = static_cast<uint16_t>(std::atoi(p_Argv[1]));
    const uint32_t s_Interval = static_cast<uint32_t>(std::atoi(p_Argv[2]));
    const uint32_t s_Duration = static_cast<uint32_t>(std::atoi(p_Argv[3]));
    const std::string s_SceneMode = p_Argc > 4 ? p_Argv[4] : "menu";
    const bool s_Effectful = p_Argc > 5 && std::string(p_Argv[5]) == "effectful";
    const std::string s_ApplyMode = p_Argc > 6 ? p_Argv[6] : "ok";
    const std::string s_PlayersMode = p_Argc > 7 ? p_Argv[7] : "default"; // M6-B1
    const std::string s_CandidateMode = p_Argc > 8 ? p_Argv[8] : "default"; // M6-D1a
    const std::string s_TransformMode = p_Argc > 9 ? p_Argv[9] : "present"; // M6-F
    const std::string s_TransformApplyMode = p_Argc > 10 ? p_Argv[10] : "ok"; // M6-F
    const std::string s_TransformLater = p_Argc > 11 ? p_Argv[11] : "held"; // M6-F

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
    std::printf("command probe: candidate-mode %s (FAKE candidate, spawn and registry-identity data for relay.inspect.candidate; never HITMAN state)\n", s_CandidateMode.c_str());
    std::printf("command probe: transform-mode %s transform-apply-mode %s transform-later %s (FAKE exact-transform words for relay.inspect.candidate_transform and candidate.transform.set; never HITMAN state)\n", s_TransformMode.c_str(), s_TransformApplyMode.c_str(), s_TransformLater.c_str());
    RelayLog::Info("command probe: transform-mode {} transform-apply-mode {} transform-later {} (FAKE exact-transform words; never HITMAN state)", s_TransformMode, s_TransformApplyMode, s_TransformLater);
    RelayLog::Info("command probe: candidate-mode {} (FAKE candidate, spawn and registry-identity data; never HITMAN state)", s_CandidateMode);
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

    // M6-F: the FAKE transform world and applier, scripted per transform-mode / transform-apply-mode / transform-later.
    ProbeTransformApplier s_Transform;
    {
        auto& w = s_Transform.inner.world;
        if (s_TransformMode == "absent") s_Transform.absent = true;
        else if (s_TransformMode == "local-hero-alias") w.distinct = false;
        else if (s_TransformMode == "wrong-brick-owner") w.owner_match = false;
        else if (s_TransformMode == "index-invalid") w.index_in_range = false;
        else if (s_TransformMode == "in-registry") w.in_registry = true;
        else if (s_TransformMode == "inconclusive-slot") w.inconclusive_slot = true;
        else if (s_TransformMode == "dirty-preimage") w.dirty = true;
        else if (s_TransformMode == "no-spatial") w.spatial = false;
        else if (s_TransformMode == "no-hitman5") w.hitman5 = false;
        else if (s_TransformMode == "hero-unavailable") s_Transform.hero_unavailable = true;
        else if (s_TransformMode == "registry-unavailable") w.registry_readable = false;
        else if (s_TransformMode == "twin-bits") { w.world.w[11] += 1; w.local.w[2] = w.world.w[11]; } // the seven-digit twin of the FAKE world's own z word
        else if (s_TransformMode == "at-p")
        {
            // M6-G1: M6-E3's r3 words (scripted copies): the candidate at P and the local hero at its start.
            std::string s_Why;
            const bool s_Ok = FloatWords::Decode("3f800000,00000000,00000000,00000000,3f800000,00000000,00000000,00000000,3f800000,c2b2fae1,416d47ae,bfc2d6ba", w.world, s_Why)
                && FloatWords::Decode("c2b2fae1,416d47ae,bfc2d6ba,80000000,00000000,80000000,3f800000", w.local, s_Why)
                && FloatWords::Decode("3e2b7674,bf7c62bd,00000000,3f7c62bd,3e2b7674,00000000,00000000,00000000,3f800000,c2a77ccf,416b27e8,bfc2d6ba", w.hero_world, s_Why)
                && FloatWords::Decode("c2a77ccf,416b27e8,bfc2d6ba,80000000,00000000,3f252b92,3f43969f", w.hero_local, s_Why);
            std::printf("command probe: transform-mode at-p: the M6-E3 r3 words %s%s\n", s_Ok ? "loaded" : "NOT loaded: ", s_Why.c_str());
        }

        using FakeTransform::Mode;
        if (s_TransformApplyMode == "unchanged") w.mode = Mode::Unchanged;
        else if (s_TransformApplyMode == "elsewhere") w.mode = Mode::Elsewhere;
        else if (s_TransformApplyMode == "axes-elsewhere") w.mode = Mode::AxesElsewhere;
        else if (s_TransformApplyMode == "local-follows") w.mode = Mode::LocalFollows;
        else if (s_TransformApplyMode == "dirty-post") w.mode = Mode::DirtyPost;
        else if (s_TransformApplyMode == "readback-missing") w.mode = Mode::ReadBackUnavailable;
        else if (s_TransformApplyMode == "post-generation") w.mode = Mode::PostGenerationChange;
        else if (s_TransformApplyMode == "post-parent") w.mode = Mode::PostParentChange;
        else if (s_TransformApplyMode == "post-entity") w.mode = Mode::PostEntityChange;
        else if (s_TransformApplyMode == "fault") w.mode = Mode::FaultDuringCall;
        else if (s_TransformApplyMode == "control-changes") w.mode = Mode::ControlChanges;
        else if (s_TransformApplyMode == "control-drifts") w.mode = Mode::ControlDrifts;
        else if (s_TransformApplyMode == "exit-after-call") s_Transform.exit_after_call = true;

        using FakeTransform::Later;
        if (s_TransformLater == "returned") w.later = Later::Returned;
        else if (s_TransformLater == "near") w.later = Later::Near;
        else if (s_TransformLater == "drifted") w.later = Later::Drifted;
        else if (s_TransformLater == "dirty") w.later = Later::Dirty;
    }

    if (g_LogMode != LogMode::None)
    {
        const bool s_Opened = OpenPassThrough();
        const auto s_Previous = RelayLog::SetRawWriterForTests(&ScriptedRawWrite);
        std::printf("command probe: scripted raw log writer installed through the test seam (previous writer %s; pass-through file %s): the first applying line will be reported as not accepted, later lines pass through; the per-process relay log file is not written while the seam is installed\n", s_Previous ? "set" : "null", s_Opened ? "open" : "NOT open");
        RelayLog::Info("command probe: scripted raw log writer installed (mode {}); this line and the following ones are in the pass-through file", s_ApplyMode);
    }

    // M7-B2: the motion mode (the DLL's [motion] wiring; see the header).
    const std::string s_MotionPortText = Env("GLACIER_PROBE_MOTION_PORT");
    const bool s_Motion = !s_MotionPortText.empty();
    const MotionScript s_Script = ParseMotionScript(Env("GLACIER_PROBE_MOTION_SCRIPT"));
    std::printf("command probe: motion %s%s; script hero-step-mm %u inexact-from %u stall-at %llu:%u stall-at-invoke %u z-drift-at %llu z-drift-at-invoke %u hero-dirty-every %u (FAKE events; never HITMAN state)\n",
        s_Motion ? "on, port " : "off", s_Motion ? s_MotionPortText.c_str() : "", s_Script.hero_step_mm, s_Script.inexact_from, static_cast<unsigned long long>(s_Script.stall_frame), s_Script.stall_ms, s_Script.stall_after_invokes,
        static_cast<unsigned long long>(s_Script.z_drift_frame), s_Script.z_drift_after_invokes, s_Script.hero_dirty_every);

    CommandQueue s_Queue;
    CommandLink::Options s_Options;
    s_Options.port = s_Port;
    s_Options.backoff_initial_ms = 500;
    s_Options.backoff_max_ms = 2000;
    s_Options.effectful = s_Effectful;
    s_Options.token = s_Token;
    s_Options.motion = s_Motion;
    CommandLink s_Link(s_Options, s_Queue, s_Adapter, MonoMs);

    std::unique_ptr<MotionLink> s_MotionLink;
    std::unique_ptr<RecordingLink> s_MotionView;
    std::unique_ptr<Motion::Controller> s_MotionController;
    if (s_Motion)
    {
        MotionLink::Options s_MotionOptions;
        s_MotionOptions.port = static_cast<uint16_t>(std::atoi(s_MotionPortText.c_str()));
        s_MotionOptions.backoff_initial_ms = 500;
        s_MotionOptions.backoff_max_ms = 2000;
        s_MotionOptions.token = s_Token;
        s_MotionLink = std::make_unique<MotionLink>(s_MotionOptions, s_Adapter, MonoMs, [&] { return s_Link.AttestedGeneration(); });
        s_MotionView = std::make_unique<RecordingLink>(*s_MotionLink);
        Motion::Controller::Options s_ControllerOptions;
        s_ControllerOptions.effectful = s_Effectful;
        s_MotionController = std::make_unique<Motion::Controller>(s_ControllerOptions, s_MotionView.get());
    }

    // Bounded teardown by the probe's owner: the probe ends when its standard input closes.
    std::atomic<bool> s_StdinClosed{false};
    std::thread s_StdinThread;
    if (Env("GLACIER_PROBE_STDIN_EXIT") == "1")
    {
        s_StdinThread = std::thread([&] {
            std::string s_Ignored;
            while (std::getline(std::cin, s_Ignored)) {}
            s_StdinClosed = true;
        });
        s_StdinThread.detach();
    }
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
        bool s_StallDone = false;
        bool s_DriftDone = false;
        std::optional<std::string> s_Cached;
        std::optional<std::string> s_ContractStarted, s_ContractEnded;
        const std::string s_Generation1 = "fake-session-" + s_Adapter.substr(0, 8) + "-1";
        const std::string s_Generation2 = "fake-session-" + s_Adapter.substr(0, 8) + "-2";
        const bool s_Mission = s_SceneMode == "mission" || s_SceneMode == "restart" || s_SceneMode == "no-contract-start" || s_SceneMode == "contract-ended" || s_SceneMode == "registry-ahead";
        FakeWorld s_World;
        s_World.setter_available = !s_SetterUnavailable;
        s_Applier.world = &s_World;
        s_Transform.world = &s_World;

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
            // M6-D1a: the FAKE candidate reader (scripted per candidate-mode; never HITMAN state)
            s_Observation.candidate_inspector = [&](const CandidateObservation::CandidateQuery& p_Query) { return FakeCandidate::Reads(s_CandidateMode, s_World.playing, s_World.registry_generation, p_Query); };
            // M6-F: the FAKE exact-transform reader (the same scripted world the fake applier mutates) and the applier
            s_Observation.transform_inspector = [&] { return s_Transform.Observe(); };
            s_Observation.transform_applier = s_Effectful ? &s_Transform : nullptr;
            s_Observation.link_generation_now = [&] { return s_Link.LinkGeneration(); };

            // M7-B2: the motion controller sits beside the scheduler on the frame.
            s_Observation.motion = s_MotionController.get();

            const bool s_StallByInvoke = s_Motion && s_Script.stall_after_invokes != 0 && !s_StallDone && s_Transform.inner.invokes >= static_cast<int>(s_Script.stall_after_invokes);
            if (s_StallByInvoke || (s_Motion && s_Script.stall_frame != 0 && s_Frame == s_Script.stall_frame))
            {
                s_StallDone = true;
                std::printf("command probe: FAKE frame callback stalled for %u ms before fake frame %llu\n", s_Script.stall_ms, static_cast<unsigned long long>(s_Frame));
                std::this_thread::sleep_for(std::chrono::milliseconds(s_Script.stall_ms));
                s_Observation.now_mono_ms = MonoMs();
            }
            if (s_Motion)
            {
                auto& w = s_Transform.inner.world;
                if (s_Script.hero_step_mm != 0)
                    w.hero_world.w[9] = FloatWords::ToWord(FloatWords::ToFloat(w.hero_world.w[9]) + static_cast<float>(s_Script.hero_step_mm) / 1000.0f);
                w.hero_dirty = s_Script.hero_dirty_every != 0 && s_Frame % s_Script.hero_dirty_every == 0;
                const bool s_DriftByInvoke = s_Script.z_drift_after_invokes != 0 && !s_DriftDone && s_Transform.inner.invokes >= static_cast<int>(s_Script.z_drift_after_invokes);
                if (s_DriftByInvoke || (s_Script.z_drift_frame != 0 && s_Frame == s_Script.z_drift_frame))
                {
                    s_DriftDone = true;
                    w.world.w[11] += 1;
                    std::printf("command probe: FAKE engine changed the candidate's z word at fake frame %llu\n", static_cast<unsigned long long>(s_Frame));
                }
                if (s_Script.inexact_from != 0 && static_cast<uint32_t>(s_Transform.inner.invokes) + 1 >= s_Script.inexact_from)
                    w.mode = FakeTransform::Mode::Elsewhere;
            }

            s_Link.SetMissionGeneration(s_Observation.mission_generation);
            s_Scheduler.Tick(s_Observation);

            if (s_MotionController)
            {
                Motion::Frame s_MotionFrame;
                s_MotionFrame.frame = s_Frame;
                s_MotionFrame.now_mono_ms = MonoMs();
                s_MotionFrame.clock = MonoMs;
                s_MotionFrame.thread_id = s_Observation.thread_id;
                s_MotionFrame.scene_available = s_Observation.scene_available;
                s_MotionFrame.scene = s_Observation.scene;
                s_MotionFrame.mission_playing = s_Observation.mission_playing;
                s_MotionFrame.mission_generation = s_Observation.mission_generation;
                s_MotionFrame.relay_transition_epoch = s_Observation.relay_transition_epoch;
                s_MotionFrame.command_link_generation_now = [&] { return s_Link.LinkGeneration(); };
                s_MotionFrame.inspector = [&] {
                    s_Transform.SyncFrame();
                    return s_Transform.inner.Current(false, s_Transform.inner.world.fresh_generation);
                };
                s_MotionFrame.applier = s_Effectful ? &s_Transform : nullptr;
                s_MotionController->Step(s_MotionFrame);
            }

            s_PrevPlaying = s_Playing;
            s_Frames = s_Frame;
            std::this_thread::sleep_for(std::chrono::milliseconds(s_Interval));
        }
    });

    // The duration is the failsafe; with GLACIER_PROBE_STDIN_EXIT the owner ends the probe earlier.
    const uint64_t s_Until = MonoMs() + s_Duration;
    while (MonoMs() < s_Until && !s_StdinClosed)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (s_StdinClosed)
        std::printf("command probe: standard input closed; stopping\n");
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
        "command probe: queue pushed %llu popped %llu busy %llu duplicates %llu cleared %llu remembered %zu; scheduler frames %llu executed %llu inspected %llu players_inspected %llu entities_inspected %llu candidates_inspected %llu outfit dispatched %llu called %llu changed %llu not_applied %llu uncertain %llu reduced %llu unbounded %llu expired %llu stale %llu rejected %llu first thread %lu last thread %lu changed %d last latency %llu ms max %llu ms; FAKE applier observes %d invokes %d readbacks %d latched %d\n",
        s_Queued.pushed, s_Queued.popped, s_Queued.busy, s_Queued.duplicates, s_Queued.cleared, s_Queued.remembered, s_Scheduled.frames, s_Scheduled.executed, s_Scheduled.inspected, s_Scheduled.players_inspected, s_Scheduled.entities_inspected, s_Scheduled.candidates_inspected, s_Scheduled.outfit_dispatched, s_Scheduled.outfit_called, s_Scheduled.outfit_changed, s_Scheduled.outfit_not_applied, s_Scheduled.outfit_uncertain, s_Scheduled.reduced, s_Scheduled.unbounded, s_Scheduled.expired, s_Scheduled.stale_generation, s_Scheduled.rejected, s_Scheduled.first_thread_id, s_Scheduled.last_thread_id, s_Scheduled.thread_changed ? 1 : 0, s_Scheduled.last_latency_ms, s_Scheduled.max_latency_ms,
        s_Applier.inner.observes, s_Applier.inner.invokes, s_Applier.inner.readbacks, s_Applier.inner.Latched() ? 1 : 0
    );
    const auto& s_Ledger = s_Scheduler.TransformLedger();
    std::printf(
        "command probe: transform inspected %llu dispatched %llu called %llu changed %llu not_applied %llu uncertain %llu; ledger state %s budget %u forward %s v%u %s link %llu effect %s invalidated '%s'; FAKE transform applier observes %d invokes %d readbacks %d latched %d\n",
        s_Scheduled.transforms_inspected, s_Scheduled.transform_dispatched, s_Scheduled.transform_called, s_Scheduled.transform_changed, s_Scheduled.transform_not_applied, s_Scheduled.transform_uncertain,
        TransformApply::StateText(s_Ledger.state), s_Ledger.forward_budget, s_Ledger.forward_id.empty() ? "-" : s_Ledger.forward_id.c_str(), s_Ledger.forward_version, s_Ledger.forward_version ? CommandProtocol::TransformModeText(s_Ledger.forward_mode) : "-", s_Ledger.link_generation, OutfitApply::EffectText(s_Ledger.forward_effect), s_Ledger.invalidated_reason.c_str(),
        s_Transform.inner.observes, s_Transform.inner.invokes, s_Transform.inner.readbacks, s_Transform.inner.Latched() ? 1 : 0
    );
    RelayLog::Info("command probe: transform inspected {} dispatched {} called {} changed {}; ledger {} budget {}", s_Scheduled.transforms_inspected, s_Scheduled.transform_dispatched, s_Scheduled.transform_called, s_Scheduled.transform_changed, TransformApply::StateText(s_Ledger.state), s_Ledger.forward_budget);
    if (s_MotionController)
    {
        const auto m = s_MotionLink->GetStats();
        std::printf(
            "command probe: motion link connects %llu disconnects %llu welcomes %llu attests %llu refusals %llu violations %llu serial %llu bound %d generation %llu command generation %llu; lines received %llu targets %llu closes %llu share closes %llu; queued control %llu full %llu samples %llu full %llu; lines sent %llu send failures %llu discarded %llu drops %llu command link changes %llu; last disconnect '%s' last refusal '%s'\n",
            m.connects, m.disconnects, m.welcomes, m.attests_sent, m.refusals, m.violations, m.serial, m.bound ? 1 : 0, m.motion_generation, m.command_generation, m.lines_received, m.targets_received, m.closes_received, m.share_closes_received,
            m.control_queued, m.control_full, m.samples_queued, m.samples_full, m.lines_sent, m.send_failures, m.discarded_on_disconnect, m.drop_requests, m.command_link_changes, m.last_disconnect.c_str(), m.last_refusal.c_str()
        );
        const auto c = s_MotionController->GetStats();
        std::printf(
            "command probe: motion controller shares %llu/%llu samples taken %llu queued %llu dropped %llu unavailable %llu dirty %llu last share close '%s'; sessions %llu/%llu targets %llu applied %llu skipped %llu refused %llu uncertain %llu invocations %llu reports %llu foreign targets %llu late targets %llu stale %llu closes for closed %llu foreign closes %llu control full %llu deadline at call %llu cancelled at call %llu deferred %llu; m6 held %llu released at baseline %llu restores %llu invoked %llu proven %llu; last session close '%s' clean %d; scheduler motion dispatched %llu transform motion held %llu\n",
            c.shares_opened, c.shares_closed, c.samples_taken, c.samples_queued, c.samples_dropped, c.samples_unavailable, c.samples_dirty, c.last_share_close.c_str(), c.sessions_opened, c.sessions_closed, c.targets_received, c.applied, c.skipped, c.refused, c.uncertain, c.invocations, c.reports_sent,
            c.foreign_targets, c.late_targets, c.stale_inbound, c.closes_for_closed, c.foreign_closes, c.control_full, c.deadline_at_call, c.cancelled_at_call, c.two_writes_prevented, c.m6_held, c.released_at_baseline, c.restores_dispatched, c.restores_invoked, c.restores_proven,
            c.last_session_close.c_str(), c.last_close_clean ? 1 : 0, s_Scheduled.motion_dispatched, s_Scheduled.transform_motion_held
        );
    }
    RelayLog::Info("command probe: done after {} fake frames; connects {}, executed {}, inspected {}, players inspected {}, entities inspected {}, candidates inspected {}, outfit dispatched {} called {} changed {}, expired {}, violations {}", s_Frames, s_Stats.connects, s_Scheduled.executed, s_Scheduled.inspected, s_Scheduled.players_inspected, s_Scheduled.entities_inspected, s_Scheduled.candidates_inspected, s_Scheduled.outfit_dispatched, s_Scheduled.outfit_called, s_Scheduled.outfit_changed, s_Scheduled.expired, s_Stats.protocol_violations);

    return s_Stats.connects > 0 ? 0 : 1;
}
