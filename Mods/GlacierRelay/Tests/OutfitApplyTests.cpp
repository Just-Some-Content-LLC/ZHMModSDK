// M5 C-2B tests: the effectful apply over a FAKE applier — every precondition branch refuses with
// its reason and never invokes; the successful case invokes exactly once and classifies changed;
// unchanged, inconsistent, generation-changed, faulted, missing-read-back outcomes are classified
// as said; the reversal; the scheduler's dispatch, expiry, duplicate, link loss and the applier-less
// rejection; the result line bound. No engine pointer is constructed, valid or invalid.
#include "TestHarness.h"

#include <string>
#include <vector>

#include <Windows.h>

#include "CommandScheduler.h"
#include "FakeOutfitApplier.h"
#include "OutfitApply.h"
#include "RelayLog.h"
#include "TestListener.h"

using namespace CommandProtocol;
using FakeOutfit::Mode;
using OutfitApply::Effect;
using OutfitApply::Invocation;

namespace
{
    constexpr const char* k_Suit = FakeOutfit::World::k_FakeSuit;
    constexpr const char* k_Target = FakeOutfit::World::k_FakeTarget;
    constexpr const char* k_Gen1 = "2516103582917844185-a105ec5f-3a75-46a3-8ecd-e72fd15b176c";
    constexpr const char* k_Gen2 = "2516103582027577704-30ccebb9-8138-4c72-9eb9-1a289b4fba33";

    OutfitTuple Tuple(const char* p_Id, int32_t p_Charset = 0, int32_t p_Variation = 0)
    {
        OutfitTuple s_Tuple;
        s_Tuple.outfit_id = p_Id;
        s_Tuple.charset = p_Charset;
        s_Tuple.variation = p_Variation;
        return s_Tuple;
    }

    Command SetOutfit(const std::string& p_Id, OutfitTuple p_Target = Tuple(k_Target), OutfitTuple p_Expect = Tuple(k_Suit), std::optional<std::string> p_Expected = k_Gen1, uint32_t p_Ttl = k_MaxTtlMs)
    {
        Command s_Command;
        s_Command.command_id = p_Id;
        s_Command.link_generation = 1;
        s_Command.ttl_ms = p_Ttl;
        s_Command.name = "player.outfit.set";
        s_Command.version = 2;
        s_Command.entry = Entry::PlayerOutfitSet2;
        s_Command.outfit_target = std::move(p_Target);
        s_Command.outfit_expect_current = std::move(p_Expect);
        s_Command.mission_generation_precondition = std::move(p_Expected);
        return s_Command;
    }

    CommandQueue::Pending Pending(const Command& p_Command, const char* p_ReceiptGen = k_Gen1, uint64_t p_ReceiptEpoch = 1)
    {
        CommandQueue::Pending s_Pending;
        s_Pending.command = p_Command;
        s_Pending.received_mono_ms = 4900;
        s_Pending.link_generation = 1;
        s_Pending.at_receipt.mission_generation = p_ReceiptGen ? std::optional<std::string>(p_ReceiptGen) : std::nullopt;
        s_Pending.at_receipt.relay_transition_epoch = p_ReceiptEpoch;
        return s_Pending;
    }

    // A stable playing frame: rise at frame 1 / t=0, now frame 100 / t=5000, epoch 1 since the rise.
    OutfitApply::Context PlayingContext(const CommandQueue::Pending& p_Pending)
    {
        OutfitApply::Context s_Context;
        s_Context.pending = &p_Pending;
        s_Context.frame = 100;
        s_Context.now_mono_ms = 5000;
        s_Context.thread_id = GetCurrentThreadId();
        s_Context.expected_thread_id = GetCurrentThreadId();
        s_Context.scene_available = true;
        s_Context.scene.available = true;
        s_Context.scene.scene_resource = "assembly:/_PRO/Scenes/Missions/Paris/_Scene_FashionShowHit_01.entity";
        s_Context.scene.scene_loaded = true;
        s_Context.scene.loading_stage = 8;
        s_Context.scene.scene_type = "mission";
        s_Context.mission_playing = true;
        s_Context.previous_frame_playing = true;
        s_Context.mission_generation = k_Gen1;
        s_Context.link_generation = 1;
        s_Context.link_generation_now = [] { return uint64_t(1); };
        s_Context.relay_transition_epoch = 1;
        s_Context.rise_frame = 1;
        s_Context.rise_mono_ms = 0;
        s_Context.scene_resource_same_as_rise = true;
        s_Context.contract_started_seen = k_Gen1;
        return s_Context;
    }

    const ObservedValue* Observed(const Result& p_Result, const char* p_Key)
    {
        for (const auto& s_Entry : p_Result.observed)
            if (s_Entry.first == p_Key)
                return &s_Entry.second;
        return nullptr;
    }

    std::string TextField(const Result& p_Result, const char* p_Key)
    {
        const auto* s_Value = Observed(p_Result, p_Key);
        return s_Value && s_Value->kind == ObservedValue::Kind::Text ? s_Value->text : std::string();
    }

    int64_t IntField(const Result& p_Result, const char* p_Key)
    {
        const auto* s_Value = Observed(p_Result, p_Key);
        return s_Value && s_Value->kind == ObservedValue::Kind::Integer && s_Value->field_status && *s_Value->field_status == FieldStatus::Observed ? s_Value->integer : INT64_MIN;
    }

    bool BoolField(const Result& p_Result, const char* p_Key, bool p_Expected)
    {
        const auto* s_Value = Observed(p_Result, p_Key);
        return s_Value && s_Value->field_status && *s_Value->field_status == FieldStatus::Observed && s_Value->kind == ObservedValue::Kind::Bool && s_Value->boolean == p_Expected;
    }

    bool FieldIs(const Result& p_Result, const char* p_Key, FieldStatus p_Status)
    {
        const auto* s_Value = Observed(p_Result, p_Key);
        return s_Value && s_Value->field_status && *s_Value->field_status == p_Status;
    }

    bool Contains(const std::string& p_Text, const std::string& p_Needle)
    {
        return p_Text.find(p_Needle) != std::string::npos;
    }

    // The scripted raw log writer for the checked "applying" line (correction B): a captureless
    // function, so its script and its record are statics. It never touches a file.
    struct LogScript
    {
        bool file_open = true;
        bool succeeded = true;
        uint32_t short_by = 0;
        uint32_t error = 0;
    };

    LogScript g_LogScript;
    int g_LogWrites = 0;
    std::vector<std::string> g_LogLines;
    const FakeOutfit::Applier* g_LogApplier = nullptr;
    int g_InvokesAtLastWrite = -1;

    RelayLog::RawWrite ScriptedRawWrite(const char* p_Data, uint32_t p_Size)
    {
        ++g_LogWrites;
        g_LogLines.emplace_back(p_Data, p_Size);
        g_InvokesAtLastWrite = g_LogApplier ? g_LogApplier->invokes : -1;

        RelayLog::RawWrite s_Raw;
        s_Raw.file_open = g_LogScript.file_open;
        s_Raw.succeeded = g_LogScript.file_open && g_LogScript.succeeded;
        s_Raw.written = s_Raw.succeeded ? (p_Size > g_LogScript.short_by ? p_Size - g_LogScript.short_by : 0) : 0;
        s_Raw.error = g_LogScript.error;
        return s_Raw;
    }

    void ResetLogScript(const FakeOutfit::Applier* p_Applier)
    {
        g_LogScript = LogScript{};
        g_LogWrites = 0;
        g_LogLines.clear();
        g_LogApplier = p_Applier;
        g_InvokesAtLastWrite = -1;
    }

    struct Run
    {
        Result result;
        OutfitApply::Outcome outcome;
    };

    Run Apply(const OutfitApply::Context& p_Context, FakeOutfit::Applier& p_Applier)
    {
        Run s_Run;
        s_Run.result.command_id = p_Context.pending->command.command_id;
        s_Run.result.link_generation = 1;
        s_Run.result.adapter_instance_id = CommandTest::Uuid(99);
        s_Run.result.native_at = "t";
        s_Run.result.frame = p_Context.frame;
        s_Run.result.mission_generation = p_Context.mission_generation;
        s_Run.outcome = OutfitApply::Apply(p_Context, p_Applier, s_Run.result);
        return s_Run;
    }
}

void RunOutfitApplyTests()
{
    // --- the successful change: exactly one invoke, classified changed, the three tuples reported ---
    {
        FakeOutfit::Applier s_Applier;
        const auto s_Pending = Pending(SetOutfit(CommandTest::Uuid(1)));
        const auto s_Run = Apply(PlayingContext(s_Pending), s_Applier);
        CHECK(s_Run.result.status == Status::Applied);
        CHECK(s_Run.outcome.invocation == Invocation::Called && s_Run.outcome.effect == Effect::Changed && s_Run.outcome.precondition_ok);
        CHECK(s_Applier.invokes == 1 && s_Applier.observes == 1 && s_Applier.readbacks == 1 && s_Applier.releases == 1);
        CHECK(s_Applier.last_candidate && *s_Applier.last_candidate == k_Target);
        CHECK(TextField(s_Run.result, "previous_outfit_id") == k_Suit && IntField(s_Run.result, "previous_charset") == 0 && IntField(s_Run.result, "previous_variation") == 0);
        CHECK(TextField(s_Run.result, "requested_outfit_id") == k_Target && IntField(s_Run.result, "requested_charset") == 0);
        CHECK(TextField(s_Run.result, "current_outfit_id") == k_Target && IntField(s_Run.result, "current_charset") == 0 && IntField(s_Run.result, "current_variation") == 0);
        CHECK(TextField(s_Run.result, "generation_fresh") == k_Gen1 && TextField(s_Run.result, "generation_post") == k_Gen1);
        CHECK(IntField(s_Run.result, "epoch_post") == 1 && IntField(s_Run.result, "link_generation_at_call") == 1);
        CHECK(BoolField(s_Run.result, "setter_called", true) && TextField(s_Run.result, "invocation") == "called" && TextField(s_Run.result, "effect") == "changed");
        CHECK(BoolField(s_Run.result, "precondition_ok", true) && BoolField(s_Run.result, "candidate_found", true) && BoolField(s_Run.result, "candidate_suit", true));
        CHECK(Contains(s_Run.result.reason, "native read-back, same frame"));
        CHECK(s_Run.result.observed.size() <= k_MaxObservedEntries);
        CHECK(EncodeResult(s_Run.result).size() <= k_MaxLineBytes);
        CHECK(!s_Run.outcome.reduced);

        // The reversal: a second, separate command with the swapped tuples on the fake state the
        // first one left; classified changed and restoring the prior tuple.
        const auto s_Reverse = Pending(SetOutfit(CommandTest::Uuid(2), Tuple(k_Suit), Tuple(k_Target)));
        const auto s_Back = Apply(PlayingContext(s_Reverse), s_Applier);
        CHECK(s_Back.result.status == Status::Applied && s_Back.outcome.effect == Effect::Changed);
        CHECK(TextField(s_Back.result, "previous_outfit_id") == k_Target && TextField(s_Back.result, "current_outfit_id") == k_Suit);
        CHECK(s_Applier.invokes == 2);
    }

    // --- every precondition branch refuses with its reason and never invokes ------------------------
    {
        struct Case
        {
            const char* label;
            const char* reason;
            bool unavailable;
            void (*arrange)(OutfitApply::Context&, CommandQueue::Pending&, FakeOutfit::Applier&);
        };

        const Case s_Cases[] = {
            {"not playing", "mission_not_playing", false, [](OutfitApply::Context& c, CommandQueue::Pending&, FakeOutfit::Applier&) { c.mission_playing = false; }},
            {"previous frame not playing", "previous_frame_not_playing", false, [](OutfitApply::Context& c, CommandQueue::Pending&, FakeOutfit::Applier&) { c.previous_frame_playing = false; }},
            {"scene unavailable", "scene_unavailable", true, [](OutfitApply::Context& c, CommandQueue::Pending&, FakeOutfit::Applier&) { c.scene_available = false; }},
            {"scene not loaded", "scene_not_loaded", false, [](OutfitApply::Context& c, CommandQueue::Pending&, FakeOutfit::Applier&) { c.scene.scene_loaded = false; }},
            {"loading stage", "loading_stage_not_playing", false, [](OutfitApply::Context& c, CommandQueue::Pending&, FakeOutfit::Applier&) { c.scene.loading_stage = 5; }},
            {"scene type", "scene_type_not_mission", false, [](OutfitApply::Context& c, CommandQueue::Pending&, FakeOutfit::Applier&) { c.scene.scene_type = ""; }},
            {"stable interval", "stable_interval_too_short", false, [](OutfitApply::Context& c, CommandQueue::Pending&, FakeOutfit::Applier&) { c.now_mono_ms = 500; c.rise_mono_ms = 0; }},
            {"stable frames", "stable_frames_too_few", false, [](OutfitApply::Context& c, CommandQueue::Pending&, FakeOutfit::Applier&) { c.rise_frame = 90; }},
            {"contract start missing", "contract_start_missing", false, [](OutfitApply::Context& c, CommandQueue::Pending&, FakeOutfit::Applier&) { c.contract_started_seen.reset(); }},
            {"contract start mismatch", "contract_start_mismatch", false, [](OutfitApply::Context& c, CommandQueue::Pending&, FakeOutfit::Applier&) { c.contract_started_seen = k_Gen2; }},
            {"contract end observed", "contract_end_observed", false, [](OutfitApply::Context& c, CommandQueue::Pending&, FakeOutfit::Applier&) { c.contract_ended_seen = k_Gen1; }},
            {"expected generation missing", "expected_generation_missing", false, [](OutfitApply::Context&, CommandQueue::Pending& p, FakeOutfit::Applier&) { p.command.mission_generation_precondition.reset(); }},
            {"receipt generation mismatch", "generation_mismatch_receipt", false, [](OutfitApply::Context&, CommandQueue::Pending& p, FakeOutfit::Applier&) { p.at_receipt.mission_generation = k_Gen2; }},
            {"receipt generation missing", "receipt_generation_missing", false, [](OutfitApply::Context&, CommandQueue::Pending& p, FakeOutfit::Applier&) { p.at_receipt.mission_generation.reset(); }},
            {"cached generation mismatch", "generation_mismatch_cached", false, [](OutfitApply::Context& c, CommandQueue::Pending&, FakeOutfit::Applier&) { c.mission_generation = k_Gen2; }},
            {"cached generation missing", "cached_generation_missing", false, [](OutfitApply::Context& c, CommandQueue::Pending&, FakeOutfit::Applier&) { c.mission_generation.reset(); }},
            {"fresh generation mismatch", "generation_mismatch_fresh", false, [](OutfitApply::Context&, CommandQueue::Pending&, FakeOutfit::Applier& a) { a.world.fresh_generation = k_Gen2; }},
            {"fresh generation unavailable", "fresh_generation_unavailable", true, [](OutfitApply::Context&, CommandQueue::Pending&, FakeOutfit::Applier& a) { a.world.fresh_unavailable = true; }},
            {"fresh generation inconsistent", "fresh_generation_inconsistent", true, [](OutfitApply::Context&, CommandQueue::Pending&, FakeOutfit::Applier& a) { a.world.fresh_inconsistent = true; }},
            {"epoch changed", "epoch_changed", false, [](OutfitApply::Context& c, CommandQueue::Pending&, FakeOutfit::Applier&) { c.relay_transition_epoch = 2; }},
            {"scene resource changed", "scene_resource_changed", false, [](OutfitApply::Context& c, CommandQueue::Pending&, FakeOutfit::Applier&) { c.scene_resource_same_as_rise = false; }},
            {"player unavailable", "player_unavailable", true, [](OutfitApply::Context&, CommandQueue::Pending&, FakeOutfit::Applier& a) { a.world.player_present = false; }},
            {"current tuple unreadable", "current_tuple_unreadable", true, [](OutfitApply::Context&, CommandQueue::Pending&, FakeOutfit::Applier& a) { a.world.hero_tuple_readable = false; }},
            {"expected-current id mismatch", "expect_current_tuple_mismatch", false, [](OutfitApply::Context&, CommandQueue::Pending& p, FakeOutfit::Applier&) { p.command.outfit_expect_current = Tuple(k_Target); }},
            {"expected-current charset mismatch", "expect_current_tuple_mismatch", false, [](OutfitApply::Context&, CommandQueue::Pending& p, FakeOutfit::Applier&) { p.command.outfit_expect_current = Tuple(k_Suit, 1, 0); }},
            {"expected-current variation mismatch", "expect_current_tuple_mismatch", false, [](OutfitApply::Context&, CommandQueue::Pending& p, FakeOutfit::Applier&) { p.command.outfit_expect_current = Tuple(k_Suit, 0, 1); }},
            {"target equals current", "target_equals_current", false, [](OutfitApply::Context&, CommandQueue::Pending& p, FakeOutfit::Applier&) { p.command.outfit_target = Tuple(k_Suit); }},
            {"candidate unresolved", "candidate_unresolved", false, [](OutfitApply::Context&, CommandQueue::Pending& p, FakeOutfit::Applier&) { p.command.outfit_target = Tuple("9c8f3a75-7054-457c-919d-ee87e77f57d9"); }},
            {"kit map unavailable", "candidate_unresolved", false, [](OutfitApply::Context&, CommandQueue::Pending&, FakeOutfit::Applier& a) { a.world.kit_map_available = false; }},
            {"target charset out of bounds", "target_charset_out_of_bounds", false, [](OutfitApply::Context&, CommandQueue::Pending& p, FakeOutfit::Applier&) { p.command.outfit_target = Tuple(k_Target, 1, 0); }},
            {"target HeroA collection invalid", "target_heroa_collection_invalid", false, [](OutfitApply::Context&, CommandQueue::Pending& p, FakeOutfit::Applier&) { p.command.outfit_target = Tuple(FakeOutfit::World::k_FakeNoHeroA, 0, 0); }},
            {"target variation out of bounds", "target_variation_out_of_bounds", false, [](OutfitApply::Context&, CommandQueue::Pending& p, FakeOutfit::Applier&) { p.command.outfit_target = Tuple(FakeOutfit::World::k_FakeTwoCharsets, 1, 3); }},
            {"setter unavailable", "setter_unavailable", true, [](OutfitApply::Context&, CommandQueue::Pending&, FakeOutfit::Applier& a) { a.world.setter_available = false; }},
            {"link generation changed", "link_generation_changed", true, [](OutfitApply::Context& c, CommandQueue::Pending&, FakeOutfit::Applier&) { c.link_generation_now = [] { return uint64_t(2); }; }},
            {"link lost", "link_generation_changed", true, [](OutfitApply::Context& c, CommandQueue::Pending&, FakeOutfit::Applier&) { c.link_generation_now = [] { return uint64_t(0); }; }},
            {"thread mismatch", "thread_mismatch", true, [](OutfitApply::Context& c, CommandQueue::Pending&, FakeOutfit::Applier&) { c.expected_thread_id = c.thread_id + 1; }},
            {"latched", "effectful_latched", true, [](OutfitApply::Context&, CommandQueue::Pending&, FakeOutfit::Applier& a) { a.latched = true; }},
            {"reads fault before the call", "player_unavailable", true, [](OutfitApply::Context&, CommandQueue::Pending&, FakeOutfit::Applier& a) { a.world.mode = Mode::ObserveFaults; }},
        };

        for (const auto& s_Case : s_Cases)
        {
            FakeOutfit::Applier s_Applier;
            auto s_Pending = Pending(SetOutfit(CommandTest::Uuid(10)));
            auto s_Context = PlayingContext(s_Pending);
            s_Case.arrange(s_Context, s_Pending, s_Applier);
            const auto s_Run = Apply(s_Context, s_Applier);
            const bool s_Status = s_Run.result.status == (s_Case.unavailable ? Status::Unavailable : Status::PreconditionFailed);
            CHECK(s_Status);
            if (!s_Status)
                std::printf("  case '%s': status %s reason %s\n", s_Case.label, StatusText(s_Run.result.status), s_Run.result.reason.c_str());
            const bool s_Reason = Contains(TextField(s_Run.result, "precondition_reasons"), s_Case.reason) || Contains(s_Run.result.reason, s_Case.reason);
            CHECK(s_Reason);
            if (!s_Reason)
                std::printf("  case '%s': reasons %s\n", s_Case.label, s_Run.result.reason.c_str());
            CHECK(s_Applier.invokes == 0 && s_Applier.readbacks == 0 && s_Applier.releases == 1);
            CHECK(s_Run.outcome.invocation == Invocation::NotCalled && s_Run.outcome.effect == Effect::None);
            CHECK(BoolField(s_Run.result, "setter_called", false) && TextField(s_Run.result, "effect") == "none" && BoolField(s_Run.result, "precondition_ok", false));
            CHECK(FieldIs(s_Run.result, "current_outfit_id", FieldStatus::Unavailable) && FieldIs(s_Run.result, "generation_post", FieldStatus::Unavailable));
            CHECK(EncodeResult(s_Run.result).size() <= k_MaxLineBytes);
        }
    }

    // --- the link generation changing between the gate and the call (the second re-read) -----------
    {
        FakeOutfit::Applier s_Applier;
        const auto s_Pending = Pending(SetOutfit(CommandTest::Uuid(11)));
        auto s_Context = PlayingContext(s_Pending);
        int s_Reads = 0;
        s_Context.link_generation_now = [&] { return ++s_Reads == 1 ? uint64_t(1) : uint64_t(0); };
        const auto s_Run = Apply(s_Context, s_Applier);
        CHECK(s_Run.result.status == Status::Unavailable && Contains(s_Run.result.reason, "link_generation_changed immediately before the call"));
        CHECK(s_Applier.invokes == 0 && s_Run.outcome.precondition_ok && s_Run.outcome.invocation == Invocation::NotCalled);
    }

    // --- the setter's other outcomes, classified as said; never "definitely unapplied" ---------------
    {
        struct Case { Mode mode; Status status; Effect effect; Invocation invocation; const char* note; };
        const Case s_Cases[] = {
            {Mode::Unchanged, Status::Applied, Effect::Unchanged, Invocation::Called, "equals the prior tuple"},
            {Mode::Elsewhere, Status::Applied, Effect::Uncertain, Invocation::Called, "matches neither"},
            {Mode::FaultDuringCall, Status::Failed, Effect::Uncertain, Invocation::Faulted, "fault inside the setter"},
            {Mode::ReadBackUnavailable, Status::Applied, Effect::Uncertain, Invocation::Called, "read-back unavailable"},
            {Mode::PostGenerationChange, Status::Applied, Effect::Uncertain, Invocation::Called, "generation changed"},
        };

        for (const auto& s_Case : s_Cases)
        {
            FakeOutfit::Applier s_Applier;
            s_Applier.world.mode = s_Case.mode;
            const auto s_Pending = Pending(SetOutfit(CommandTest::Uuid(12)));
            const auto s_Run = Apply(PlayingContext(s_Pending), s_Applier);
            CHECK(s_Run.result.status == s_Case.status && s_Run.outcome.effect == s_Case.effect && s_Run.outcome.invocation == s_Case.invocation);
            CHECK(s_Applier.invokes == 1 && s_Applier.readbacks == 1 && s_Applier.releases == 1);
            CHECK(Contains(s_Run.result.reason, s_Case.note));
            CHECK(BoolField(s_Run.result, "setter_called", true) && TextField(s_Run.result, "effect") == OutfitApply::EffectText(s_Case.effect));
            CHECK(TextField(s_Run.result, "previous_outfit_id") == k_Suit);
            if (s_Case.mode == Mode::ReadBackUnavailable)
                CHECK(FieldIs(s_Run.result, "current_charset", FieldStatus::Unavailable) && TextField(s_Run.result, "current_outfit_id") == k_Target);
            if (s_Case.mode == Mode::PostGenerationChange)
                CHECK(TextField(s_Run.result, "generation_post") == k_Gen2);
            if (s_Case.mode == Mode::FaultDuringCall)
            {
                CHECK(s_Applier.Latched());
                // After a fault the path is latched: the next command never reaches the setter.
                const auto s_Next = Apply(PlayingContext(s_Pending), s_Applier);
                CHECK(s_Next.result.status == Status::Unavailable && Contains(s_Next.result.reason, "effectful_latched") && s_Applier.invokes == 1);
            }
        }
    }

    // --- the checked "applying" line before the one call (correction B, M5 design section 30) ------
    {
        const RelayLog::RawWriter s_PreviousWriter = RelayLog::SetRawWriterForTests(&ScriptedRawWrite);
        CHECK(s_PreviousWriter == nullptr);

        // RelayLog's own classification of the four raw outcomes, with the byte counts and the error.
        {
            ResetLogScript(nullptr);
            auto s_Outcome = RelayLog::WriteChecked(RelayLog::Level::Info, "checked write probe");
            CHECK(s_Outcome.status == RelayLog::WriteStatus::Written && s_Outcome.written == s_Outcome.requested && s_Outcome.requested > 0 && s_Outcome.error == 0);
            CHECK(g_LogWrites == 1 && Contains(g_LogLines[0], "checked write probe\n") && g_LogLines[0].size() == s_Outcome.requested);

            g_LogScript.file_open = false;
            s_Outcome = RelayLog::WriteChecked(RelayLog::Level::Info, "checked write probe");
            CHECK(s_Outcome.status == RelayLog::WriteStatus::FileUnavailable && s_Outcome.written == 0);

            g_LogScript = LogScript{};
            g_LogScript.succeeded = false;
            g_LogScript.error = 5;
            s_Outcome = RelayLog::WriteChecked(RelayLog::Level::Info, "checked write probe");
            CHECK(s_Outcome.status == RelayLog::WriteStatus::WriteFailed && s_Outcome.written == 0 && s_Outcome.error == 5);

            g_LogScript = LogScript{};
            g_LogScript.short_by = 7;
            s_Outcome = RelayLog::WriteChecked(RelayLog::Level::Info, "checked write probe");
            CHECK(s_Outcome.status == RelayLog::WriteStatus::ShortWrite && s_Outcome.written + 7 == s_Outcome.requested);
            CHECK(std::string(RelayLog::WriteStatusText(RelayLog::WriteStatus::Written)) == "written" && std::string(RelayLog::WriteStatusText(RelayLog::WriteStatus::ShortWrite)) == "short_write");
        }

        // The successful case: exactly one applying line, accepted in full, written before the one
        // invoke; the classification is the one the earlier success case established.
        {
            FakeOutfit::Applier s_Applier;
            ResetLogScript(&s_Applier);
            const auto s_Pending = Pending(SetOutfit(CommandTest::Uuid(40)));
            const auto s_Run = Apply(PlayingContext(s_Pending), s_Applier);
            CHECK(s_Run.result.status == Status::Applied && s_Run.outcome.invocation == Invocation::Called && s_Run.outcome.effect == Effect::Changed);
            CHECK(s_Applier.invokes == 1 && s_Applier.readbacks == 1 && s_Applier.releases == 1);
            CHECK(g_LogWrites == 1 && g_InvokesAtLastWrite == 0);
            CHECK(Contains(g_LogLines[0], "applying player.outfit.set/2 on frame 100") && Contains(g_LogLines[0], CommandTest::Uuid(40)) && Contains(g_LogLines[0], "-> requested " + std::string(k_Target) + "/0/0"));
            CHECK(TextField(s_Run.result, "apply_log") == "written" && BoolField(s_Run.result, "setter_called", true) && TextField(s_Run.result, "effect") == "changed");
            CHECK(EncodeResult(s_Run.result).size() <= k_MaxLineBytes && !s_Run.outcome.reduced);
        }

        // The three ways the line can fail to be accepted: unavailable, apply_log_unwritten, the
        // setter never invoked, no read-back, the effect none, the gate's verdict still reported.
        {
            struct Case { const char* label; const char* status_text; void (*arrange)(); };
            const Case s_Cases[] = {
                {"file-open failure", "file_unavailable", [] { g_LogScript.file_open = false; }},
                {"failed write", "write_failed", [] { g_LogScript.succeeded = false; g_LogScript.error = 1784; }},
                {"short write", "short_write", [] { g_LogScript.short_by = 1; }},
            };

            for (const auto& s_Case : s_Cases)
            {
                FakeOutfit::Applier s_Applier;
                ResetLogScript(&s_Applier);
                s_Case.arrange();
                const auto s_Pending = Pending(SetOutfit(CommandTest::Uuid(41)));
                const auto s_Run = Apply(PlayingContext(s_Pending), s_Applier);
                const bool s_Status = s_Run.result.status == Status::Unavailable && Contains(s_Run.result.reason, "apply_log_unwritten") && Contains(s_Run.result.reason, s_Case.status_text) && Contains(s_Run.result.reason, "the setter was not called");
                CHECK(s_Status);
                if (!s_Status)
                    std::printf("  case '%s': status %s reason %s\n", s_Case.label, StatusText(s_Run.result.status), s_Run.result.reason.c_str());
                CHECK(s_Applier.invokes == 0 && s_Applier.readbacks == 0 && s_Applier.observes == 1 && s_Applier.releases == 1);
                CHECK(g_LogWrites == 1 && Contains(g_LogLines[0], CommandTest::Uuid(41)));
                CHECK(s_Run.outcome.invocation == Invocation::NotCalled && s_Run.outcome.effect == Effect::None && s_Run.outcome.precondition_ok);
                CHECK(BoolField(s_Run.result, "setter_called", false) && TextField(s_Run.result, "invocation") == "not_called" && TextField(s_Run.result, "effect") == "none");
                CHECK(TextField(s_Run.result, "apply_log") == s_Case.status_text && BoolField(s_Run.result, "precondition_ok", true));
                CHECK(FieldIs(s_Run.result, "current_outfit_id", FieldStatus::Unavailable) && FieldIs(s_Run.result, "generation_post", FieldStatus::Unavailable));
                CHECK(TextField(s_Run.result, "previous_outfit_id") == k_Suit && TextField(s_Run.result, "requested_outfit_id") == k_Target);
                CHECK(EncodeResult(s_Run.result).size() <= k_MaxLineBytes && !s_Run.outcome.reduced);
                if (s_Case.status_text == std::string("write_failed"))
                    CHECK(Contains(s_Run.result.reason, "error 1784"));

                // The path is not latched by a log failure: the next command with a working log applies.
                ResetLogScript(&s_Applier);
                const auto s_Next = Apply(PlayingContext(s_Pending), s_Applier);
                CHECK(s_Next.result.status == Status::Applied && s_Applier.invokes == 1 && g_LogWrites == 1);
            }
        }

        // A refusal by the gate writes no applying line (nothing is logged before the verdict), and
        // on the default-disabled path (no applier on the frame) the scheduler writes nothing at
        // all while rejecting the effectful command.
        {
            FakeOutfit::Applier s_Applier;
            ResetLogScript(&s_Applier);
            auto s_Pending = Pending(SetOutfit(CommandTest::Uuid(42)));
            auto s_Context = PlayingContext(s_Pending);
            s_Context.mission_playing = false;
            const auto s_Run = Apply(s_Context, s_Applier);
            CHECK(s_Run.result.status == Status::PreconditionFailed && g_LogWrites == 0 && s_Applier.invokes == 0);
            CHECK(TextField(s_Run.result, "apply_log") == "not_attempted");

            CommandQueue s_Queue;
            std::vector<Result> s_Results;
            CommandScheduler s_Scheduler(s_Queue, [&](const Result& p_Result) { s_Results.push_back(p_Result); });
            CommandQueue::FrameSnapshot s_Snapshot;
            s_Snapshot.mission_generation = k_Gen1;
            s_Snapshot.relay_transition_epoch = 1;
            s_Queue.PublishSnapshot(s_Snapshot);
            s_Queue.Push(SetOutfit(CommandTest::Uuid(43)), 1, 4900);

            CommandScheduler::FrameObservation s_Observation;
            s_Observation.frame = 100;
            s_Observation.now_mono_ms = 5000;
            s_Observation.native_at = "t";
            s_Observation.thread_id = GetCurrentThreadId();
            s_Observation.scene_available = true;
            s_Observation.scene.available = true;
            s_Observation.scene.scene_loaded = true;
            s_Observation.scene.loading_stage = 8;
            s_Observation.scene.scene_type = "mission";
            s_Observation.mission_playing = true;
            s_Observation.previous_frame_playing = true;
            s_Observation.mission_generation = k_Gen1;
            s_Observation.link_generation = 1;
            s_Observation.adapter_instance_id = CommandTest::Uuid(99);
            s_Observation.relay_transition_epoch = 1;
            s_Observation.rise_frame = 1;
            s_Observation.applier = nullptr;
            s_Observation.link_generation_now = [] { return uint64_t(1); };

            ResetLogScript(nullptr);
            CHECK(s_Scheduler.Tick(s_Observation));
            CHECK(s_Results.size() == 1 && s_Results[0].status == Status::Rejected && Contains(s_Results[0].reason, "no outfit applier"));
            CHECK(g_LogWrites == 0);
        }

        CHECK(RelayLog::SetRawWriterForTests(s_PreviousWriter) == &ScriptedRawWrite);
        ResetLogScript(nullptr);
    }

    // --- through the scheduler: dispatch, expiry, duplicate, link loss, no applier -------------------
    {
        CommandQueue s_Queue;
        std::vector<Result> s_Results;
        CommandScheduler s_Scheduler(s_Queue, [&](const Result& p_Result) { s_Results.push_back(p_Result); });
        FakeOutfit::Applier s_Applier;

        auto s_Frame = [&](uint64_t p_Frame, uint64_t p_NowMs, uint64_t p_Generation, bool p_WithApplier) {
            CommandScheduler::FrameObservation s_Observation;
            s_Observation.frame = p_Frame;
            s_Observation.now_mono_ms = p_NowMs;
            s_Observation.native_at = "t";
            s_Observation.thread_id = GetCurrentThreadId();
            s_Observation.scene_available = true;
            s_Observation.scene.available = true;
            s_Observation.scene.scene_resource = "assembly:/_PRO/Scenes/Missions/Paris/_Scene_FashionShowHit_01.entity";
            s_Observation.scene.scene_loaded = true;
            s_Observation.scene.loading_stage = 8;
            s_Observation.scene.scene_type = "mission";
            s_Observation.mission_playing = true;
            s_Observation.previous_frame_playing = true;
            s_Observation.mission_generation = k_Gen1;
            s_Observation.link_generation = p_Generation;
            s_Observation.adapter_instance_id = CommandTest::Uuid(99);
            s_Observation.relay_transition_epoch = 1;
            s_Observation.rise_frame = 1;
            s_Observation.rise_mono_ms = 0;
            s_Observation.scene_resource_same_as_rise = true;
            s_Observation.contract_started_seen = k_Gen1;
            s_Observation.applier = p_WithApplier ? &s_Applier : nullptr;
            s_Observation.link_generation_now = [p_Generation] { return p_Generation; };
            return s_Observation;
        };

        // Publish the snapshot the receipts copy (generation, epoch 1), then queue and apply.
        CommandQueue::FrameSnapshot s_Snapshot;
        s_Snapshot.mission_generation = k_Gen1;
        s_Snapshot.relay_transition_epoch = 1;
        s_Queue.PublishSnapshot(s_Snapshot);

        // No applier on the frame source: rejected, never invoked.
        s_Queue.Push(SetOutfit(CommandTest::Uuid(20)), 1, 4900);
        CHECK(s_Scheduler.Tick(s_Frame(100, 5000, 1, false)));
        CHECK(s_Results.size() == 1 && s_Results[0].status == Status::Rejected && Contains(s_Results[0].reason, "no outfit applier"));
        CHECK(s_Applier.invokes == 0 && s_Scheduler.GetStats().outfit_dispatched == 1 && s_Scheduler.GetStats().rejected == 1);

        // Applied through the scheduler; the id is remembered as applied (a resend is a duplicate).
        s_Queue.Push(SetOutfit(CommandTest::Uuid(21)), 1, 4900);
        CHECK(s_Scheduler.Tick(s_Frame(101, 5010, 1, true)));
        CHECK(s_Results.size() == 2 && s_Results[1].status == Status::Applied && s_Applier.invokes == 1);
        CHECK(s_Queue.Push(SetOutfit(CommandTest::Uuid(21)), 1, 5020).outcome == CommandQueue::PushOutcome::DuplicateDone);
        CHECK(s_Scheduler.GetStats().outfit_called == 1 && s_Scheduler.GetStats().outfit_changed == 1);

        // Expired before dispatch: never invoked, remembered as expired.
        s_Queue.Push(SetOutfit(CommandTest::Uuid(22), Tuple(k_Suit), Tuple(k_Target), k_Gen1, 100), 1, 1000);
        CHECK(s_Scheduler.Tick(s_Frame(102, 5200, 1, true)));
        CHECK(s_Results.size() == 3 && s_Results[2].status == Status::Expired && s_Applier.invokes == 1);
        CHECK(s_Queue.Push(SetOutfit(CommandTest::Uuid(22), Tuple(k_Suit), Tuple(k_Target)), 1, 5300).outcome == CommandQueue::PushOutcome::DuplicateDone);

        // Link loss with the command queued: cleared, never applied, no result; on the new link the
        // old generation is dropped without a result.
        s_Queue.Push(SetOutfit(CommandTest::Uuid(23), Tuple(k_Suit), Tuple(k_Target)), 1, 5300);
        CHECK(s_Queue.HasPendingEffectful());
        CHECK(s_Queue.Clear().size() == 1 && !s_Queue.HasPendingEffectful());
        CHECK(!s_Scheduler.Tick(s_Frame(103, 5400, 0, true)));
        s_Queue.Push(SetOutfit(CommandTest::Uuid(24), Tuple(k_Suit), Tuple(k_Target)), 1, 5400);
        CHECK(s_Scheduler.Tick(s_Frame(104, 5500, 2, true)));
        CHECK(s_Results.size() == 3 && s_Scheduler.GetStats().stale_generation == 1 && s_Applier.invokes == 1);

        // A frame whose thread is not the scheduler's first: refused, never invoked.
        s_Queue.Push(SetOutfit(CommandTest::Uuid(25), Tuple(k_Suit), Tuple(k_Target)), 1, 5500);
        std::thread s_Other([&] { s_Scheduler.Tick(s_Frame(105, 5600, 1, true)); });
        s_Other.join();
        CHECK(s_Results.size() == 4 && s_Results[3].status == Status::Unavailable && Contains(s_Results[3].reason, "thread_mismatch") && s_Applier.invokes == 1);
        CHECK(s_Scheduler.GetStats().outfit_not_applied == 1);
    }

    // --- the result line bound: the fullest result (every field, long reasons) still fits ---------
    {
        FakeOutfit::Applier s_Applier;
        s_Applier.world.mode = Mode::Elsewhere;
        auto s_Pending = Pending(SetOutfit(CommandTest::Uuid(30), Tuple(FakeOutfit::World::k_FakeTwoCharsets, 1, 2)));
        s_Pending.command.mission_generation_precondition = std::string(64, 'g');
        s_Pending.at_receipt.mission_generation = std::string(64, 'g');
        auto s_Context = PlayingContext(s_Pending);
        s_Context.mission_generation = std::string(64, 'g');
        s_Context.contract_started_seen = std::string(64, 'g');
        s_Context.contract_ended_seen = std::string(64, 'h');
        s_Applier.world.fresh_generation = std::string(64, 'g');
        s_Applier.world.post_generation = std::string(64, 'g');
        const auto s_Run = Apply(s_Context, s_Applier);
        const size_t s_Size = EncodeResult(s_Run.result).size();
        CHECK(s_Size <= k_MaxLineBytes && !s_Run.outcome.reduced);
        std::printf("OutfitApplyTests: fullest applied result encodes to %zu bytes (bound %zu, %zu fields)\n", s_Size, k_MaxLineBytes, s_Run.result.observed.size());

        // The fullest refusal (every rule failing at once) fits as well.
        FakeOutfit::Applier s_Refused;
        s_Refused.world.player_present = false;
        s_Refused.world.setter_available = false;
        s_Refused.world.fresh_inconsistent = true;
        auto s_Bad = Pending(SetOutfit(CommandTest::Uuid(31), Tuple(FakeOutfit::World::k_FakeNoHeroA, 3, 9), Tuple(k_Target, 2, 2)), nullptr, 0);
        auto s_BadContext = PlayingContext(s_Bad);
        s_BadContext.mission_playing = false;
        s_BadContext.scene.scene_loaded = false;
        s_BadContext.scene.loading_stage = 2;
        s_BadContext.scene.scene_type = "";
        s_BadContext.scene_resource_same_as_rise = false;
        s_BadContext.mission_generation.reset();
        s_BadContext.contract_started_seen = k_Gen2;
        s_BadContext.contract_ended_seen = k_Gen1;
        s_BadContext.relay_transition_epoch = 7;
        s_BadContext.link_generation_now = [] { return uint64_t(0); };
        s_BadContext.expected_thread_id = 12345;
        s_Refused.latched = true;
        const auto s_BadRun = Apply(s_BadContext, s_Refused);
        const size_t s_BadSize = EncodeResult(s_BadRun.result).size();
        CHECK(s_BadSize <= k_MaxLineBytes && s_BadRun.result.status == Status::Unavailable && s_Refused.invokes == 0);
        std::printf("OutfitApplyTests: fullest refused result encodes to %zu bytes (%zu fields)\n", s_BadSize, s_BadRun.result.observed.size());
    }
}
