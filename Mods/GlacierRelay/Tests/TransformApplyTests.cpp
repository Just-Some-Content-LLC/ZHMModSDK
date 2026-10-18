// M6-F tests: the transform apply over a FAKE applier — the qualified lift invokes exactly once and
// classifies changed over all twelve words; every identity, preimage, expectation, target, budget
// and record branch refuses with its reason and never invokes; the setter's other outcomes
// (unchanged, elsewhere, axes elsewhere, local follows, dirty post, unavailable read-back, post
// generation / parent / entity changes, fault) are classified as said; the checked "applying" line
// refusals consume no budget; the control change invalidates the ledger; the restore binds to the
// record and refuses every substitution; the mandatory result's capacity; the scheduler's dispatch.
// No engine pointer is constructed, valid or invalid.
#include "TestHarness.h"

#include <string>
#include <vector>

#include <Windows.h>

#include "CommandScheduler.h"
#include "FakeTransformApplier.h"
#include "RelayLog.h"
#include "TestListener.h"
#include "TransformApply.h"

using namespace CommandProtocol;
using FakeTransform::Later;
using FakeTransform::Mode;
using FloatWords::Words12;
using FloatWords::Words7;
using OutfitApply::Effect;
using OutfitApply::Invocation;
using TransformApply::Ledger;

namespace
{
    constexpr const char* k_Gen1 = FakeTransform::k_Gen1;
    constexpr const char* k_Gen2 = FakeTransform::k_Gen2;
    constexpr const char* k_Original = "9c8f3a75-7054-457c-919d-ee87e77f57d9";

    const Words12 k_PreWorld = FakeTransform::WorldOf(-40.99882f, -24.293657f, 0.4925565f); // the FAKE world's candidate
    const Words7 k_PreLocal = FakeTransform::LocalOf(-40.99882f, -24.293657f, 0.4925565f);
    const Words12 k_Target = TransformObservation::LiftTarget(k_PreWorld);

    Command Lift(const std::string& p_Id, Words12 p_ExpectWorld = k_PreWorld, Words7 p_ExpectLocal = k_PreLocal, std::string p_Parent = TransformObservation::k_Brick, std::optional<std::string> p_Expected = k_Gen1)
    {
        Command s_Command;
        s_Command.command_id = p_Id;
        s_Command.link_generation = 1;
        s_Command.ttl_ms = k_MaxTtlMs;
        s_Command.name = "candidate.transform.set";
        s_Command.version = 1;
        s_Command.entry = Entry::CandidateTransformSet1;
        TransformRequest s_Request;
        s_Request.mode = TransformMode::Lift;
        s_Request.expect_world = p_ExpectWorld;
        s_Request.expect_local = p_ExpectLocal;
        s_Request.expect_parent = std::move(p_Parent);
        s_Command.transform = s_Request;
        s_Command.mission_generation_precondition = std::move(p_Expected);
        s_Command.params.emplace_back("mode", "lift");
        return s_Command;
    }

    // M6-G1: a candidate.transform.set v2 step_y (the request carries its entry version).
    Command Step(const std::string& p_Id, Words12 p_ExpectWorld = k_PreWorld, Words7 p_ExpectLocal = k_PreLocal, std::string p_Parent = TransformObservation::k_Brick, std::optional<std::string> p_Expected = k_Gen1)
    {
        Command s_Command = Lift(p_Id, p_ExpectWorld, p_ExpectLocal, std::move(p_Parent), std::move(p_Expected));
        s_Command.version = 2;
        s_Command.entry = Entry::CandidateTransformSet2;
        s_Command.transform->version = 2;
        s_Command.transform->mode = TransformMode::StepY;
        s_Command.params[0].second = "step_y";
        return s_Command;
    }

    Command Restore(const std::string& p_Id, Words12 p_ExpectWorld, Words12 p_TargetWorld, std::string p_RestoreOf = k_Original, Words7 p_ExpectLocal = k_PreLocal, std::optional<std::string> p_Expected = k_Gen1)
    {
        Command s_Command = Lift(p_Id, p_ExpectWorld, p_ExpectLocal, TransformObservation::k_Brick, std::move(p_Expected));
        s_Command.transform->mode = TransformMode::Restore;
        s_Command.transform->target_world = p_TargetWorld;
        s_Command.transform->restore_of = std::move(p_RestoreOf);
        s_Command.params[0].second = "restore";
        return s_Command;
    }

    // M6-G1: a v2 restore (the request carries entry version 2).
    Command RestoreV2(const std::string& p_Id, Words12 p_ExpectWorld, Words12 p_TargetWorld, std::string p_RestoreOf, Words7 p_ExpectLocal)
    {
        Command s_Command = Restore(p_Id, p_ExpectWorld, p_TargetWorld, std::move(p_RestoreOf), p_ExpectLocal);
        s_Command.version = 2;
        s_Command.entry = Entry::CandidateTransformSet2;
        s_Command.transform->version = 2;
        return s_Command;
    }

    Words12 W12(const char* p_Text)
    {
        Words12 s_Words;
        std::string s_Why;
        CHECK(FloatWords::Decode(p_Text, s_Words, s_Why));
        return s_Words;
    }

    Words7 W7(const char* p_Text)
    {
        Words7 s_Words;
        std::string s_Why;
        CHECK(FloatWords::Decode(p_Text, s_Words, s_Why));
        return s_Words;
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
    TransformApply::Context PlayingContext(const CommandQueue::Pending& p_Pending, const char* p_Generation = k_Gen1)
    {
        TransformApply::Context s_Context;
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
        s_Context.mission_generation = p_Generation;
        s_Context.link_generation = 1;
        s_Context.link_generation_now = [] { return uint64_t(1); };
        s_Context.relay_transition_epoch = 1;
        s_Context.rise_frame = 1;
        s_Context.rise_mono_ms = 0;
        s_Context.scene_resource_same_as_rise = true;
        s_Context.contract_started_seen = p_Generation;
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
        return s_Value && s_Value->kind == ObservedValue::Kind::Text && s_Value->field_status && *s_Value->field_status == FieldStatus::Observed ? s_Value->text : std::string();
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
    const FakeTransform::Applier* g_LogApplier = nullptr;
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

    void ResetLogScript(const FakeTransform::Applier* p_Applier)
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
        TransformApply::Outcome outcome;
    };

    Run Apply(const TransformApply::Context& p_Context, FakeTransform::Applier& p_Applier, Ledger& p_Ledger)
    {
        Run s_Run;
        s_Run.result.protocol_version = 2;
        s_Run.result.command_id = p_Context.pending->command.command_id;
        s_Run.result.link_generation = 1;
        s_Run.result.adapter_instance_id = CommandTest::Uuid(99);
        s_Run.result.native_at = "2026-10-17T00:00:00.000000Z";
        s_Run.result.frame = p_Context.frame;
        s_Run.result.mission_generation = p_Context.mission_generation;
        s_Run.outcome = TransformApply::Apply(p_Context, p_Applier, p_Ledger, s_Run.result);
        return s_Run;
    }

    // A lift that succeeded on a fresh fake world and ledger (the starting point of the restore cases).
    Run LiftOnce(FakeTransform::Applier& p_Applier, Ledger& p_Ledger, const std::string& p_Id = k_Original)
    {
        const auto s_Pending = Pending(Lift(p_Id));
        return Apply(PlayingContext(s_Pending), p_Applier, p_Ledger);
    }
}

void RunTransformApplyTests()
{
    // --- the qualified lift: exactly one invoke with the compiled target, changed over all twelve words ---
    {
        FakeTransform::Applier s_Applier;
        Ledger s_Ledger;
        const auto s_Run = LiftOnce(s_Applier, s_Ledger);
        CHECK(s_Run.result.status == Status::Applied);
        CHECK(s_Run.outcome.invocation == Invocation::Called && s_Run.outcome.effect == Effect::Changed && s_Run.outcome.precondition_ok);
        CHECK(s_Applier.invokes == 1 && s_Applier.observes == 1 && s_Applier.readbacks == 1 && s_Applier.releases == 1);
        CHECK(s_Applier.last_target == k_Target);
        CHECK(TextField(s_Run.result, "mode") == "lift");
        CHECK(TextField(s_Run.result, "preimage_world_words") == FloatWords::Encode(k_PreWorld) && TextField(s_Run.result, "preimage_local_words") == FloatWords::Encode(k_PreLocal));
        CHECK(TextField(s_Run.result, "preimage_parent") == TransformObservation::k_Brick && BoolField(s_Run.result, "preimage_dirty", false));
        CHECK(TextField(s_Run.result, "preimage_world") == "-40.99882,-24.29366,0.4925565" && TextField(s_Run.result, "requested_world") == "-40.99882,-24.29366,0.7425565");
        CHECK(TextField(s_Run.result, "requested_world_words") == FloatWords::Encode(k_Target));
        CHECK(TextField(s_Run.result, "postimage_world_words") == FloatWords::Encode(k_Target) && TextField(s_Run.result, "postimage_local_words") == FloatWords::Encode(k_PreLocal));
        CHECK(BoolField(s_Run.result, "postimage_dirty", false) && BoolField(s_Run.result, "post_same_entity", true));
        CHECK(TextField(s_Run.result, "world_match") == "target" && TextField(s_Run.result, "local_match") == "unchanged" && TextField(s_Run.result, "hero_control") == "unchanged");
        CHECK(TextField(s_Run.result, "generation_fresh") == k_Gen1 && TextField(s_Run.result, "generation_post") == k_Gen1);
        CHECK(IntField(s_Run.result, "link_generation_at_call") == 1 && IntField(s_Run.result, "budget_remaining") == 0);
        CHECK(BoolField(s_Run.result, "setter_called", true) && TextField(s_Run.result, "invocation") == "called" && TextField(s_Run.result, "effect") == "changed" && TextField(s_Run.result, "apply_log") == "written");
        CHECK(BoolField(s_Run.result, "precondition_ok", true) && FieldIs(s_Run.result, "restore_facts", FieldStatus::Unavailable));
        CHECK(TextField(s_Run.result, "targets") == std::string(TransformObservation::k_Brick) + "," + TransformObservation::k_Entity);
        CHECK(TextField(s_Run.result, "registry_slots") == FakeTransform::k_LocalOnly && BoolField(s_Run.result, "cand_spatial_distinct", true) && BoolField(s_Run.result, "cand_distinct", true));
        CHECK(Contains(s_Run.result.reason, "all twelve world words equal the requested words"));
        CHECK(s_Run.result.observed.size() == 47 && s_Run.result.observed.size() <= k_MaxObservedEntries);
        CHECK(EncodeResult(s_Run.result).size() <= k_MaxLineBytes && !s_Run.outcome.reduced);
        std::printf("line bound: candidate.transform.set lift result = %zu bytes of %zu (%zu entries)\n", EncodeResult(s_Run.result).size(), k_MaxLineBytes, s_Run.result.observed.size());

        // The ledger after the call.
        CHECK(s_Ledger.state == Ledger::State::ForwardInvoked && s_Ledger.forward_budget == 0 && s_Ledger.forward_id == k_Original);
        CHECK(s_Ledger.preimage_world == k_PreWorld && s_Ledger.requested_world == k_Target && s_Ledger.postimage_world == k_Target && s_Ledger.forward_effect == Effect::Changed);
        CHECK(s_Ledger.link_generation == 1 && s_Ledger.mission_generation == k_Gen1 && !s_Ledger.postimage_dirty);

        // A second lift, fresh id, same frame shape: the budget refuses it; nothing is invoked.
        const auto s_Second = Pending(Lift(CommandTest::Uuid(2), k_Target));
        const auto s_Again = Apply(PlayingContext(s_Second), s_Applier, s_Ledger);
        CHECK(s_Again.result.status == Status::PreconditionFailed && Contains(s_Again.result.reason, "budget_exhausted") && s_Applier.invokes == 1);
        CHECK(IntField(s_Again.result, "budget_remaining") == 0 && !BoolField(s_Again.result, "setter_called", true) && TextField(s_Again.result, "apply_log") == "not_attempted");

        // The restore, bound to the record: invoked once more, changed back, the restore facts exact.
        const auto s_RestorePending = Pending(Restore(CommandTest::Uuid(3), k_Target, k_PreWorld));
        const auto s_Back = Apply(PlayingContext(s_RestorePending), s_Applier, s_Ledger);
        CHECK(s_Back.result.status == Status::Applied && s_Back.outcome.effect == Effect::Changed && s_Applier.invokes == 2 && s_Applier.last_target == k_PreWorld);
        CHECK(TextField(s_Back.result, "mode") == "restore" && TextField(s_Back.result, "restore_of") == k_Original);
        CHECK(TextField(s_Back.result, "preimage_world_words") == FloatWords::Encode(k_Target) && TextField(s_Back.result, "postimage_world_words") == FloatWords::Encode(k_PreWorld));
        CHECK(TextField(s_Back.result, "restore_facts") == "world=1,local=1,parent=1,dirty=1,snapshot=1,within_tol=1");
        CHECK(s_Ledger.state == Ledger::State::Restored);
        CHECK(EncodeResult(s_Back.result).size() <= k_MaxLineBytes && s_Back.result.observed.size() == 47);

        // A second restore: refused (one restore per forward); a third lift: still refused.
        const auto s_RestoreAgain = Pending(Restore(CommandTest::Uuid(4), k_PreWorld, k_Target));
        const auto s_Twice = Apply(PlayingContext(s_RestoreAgain), s_Applier, s_Ledger);
        CHECK(s_Twice.result.status == Status::PreconditionFailed && Contains(s_Twice.result.reason, "already_restored") && s_Applier.invokes == 2);
        const auto s_Third = Apply(PlayingContext(Pending(Lift(CommandTest::Uuid(5)))), s_Applier, s_Ledger);
        CHECK(s_Third.result.status == Status::PreconditionFailed && Contains(s_Third.result.reason, "budget_exhausted") && s_Applier.invokes == 2);
    }

    // --- every transform rule refuses with its reason and never invokes ------------------------------
    {
        struct Case
        {
            const char* label;
            const char* reason;
            bool unavailable;
            void (*arrange)(TransformApply::Context&, CommandQueue::Pending&, FakeTransform::Applier&, Ledger&);
        };

        const Case s_Cases[] = {
            {"brick absent", "candidate_brick_absent", false, [](TransformApply::Context&, CommandQueue::Pending&, FakeTransform::Applier& a, Ledger&) { a.world.brick_present = false; }},
            {"scan incomplete", "candidate_brick_absent", false, [](TransformApply::Context&, CommandQueue::Pending&, FakeTransform::Applier& a, Ledger&) { a.world.brick_present = false; a.world.scan_complete = false; }},
            {"unresolved", "candidate_unresolved", false, [](TransformApply::Context&, CommandQueue::Pending&, FakeTransform::Applier& a, Ledger&) { a.world.resolved = false; }},
            {"index out of range", "candidate_index_not_in_range", false, [](TransformApply::Context&, CommandQueue::Pending&, FakeTransform::Applier& a, Ledger&) { a.world.index_in_range = false; }},
            {"no hero interface", "candidate_not_hitman5", false, [](TransformApply::Context&, CommandQueue::Pending&, FakeTransform::Applier& a, Ledger&) { a.world.hitman5 = false; }},
            {"id mismatch", "candidate_id_mismatch", false, [](TransformApply::Context&, CommandQueue::Pending&, FakeTransform::Applier& a, Ledger&) { a.world.id_match = false; }},
            {"wrong owning brick", "candidate_owner_mismatch", false, [](TransformApply::Context&, CommandQueue::Pending&, FakeTransform::Applier& a, Ledger&) { a.world.owner_match = false; }},
            {"aliases the local hero", "candidate_not_distinct", false, [](TransformApply::Context&, CommandQueue::Pending&, FakeTransform::Applier& a, Ledger&) { a.world.distinct = false; }},
            {"no spatial interface", "candidate_spatial_unavailable", true, [](TransformApply::Context&, CommandQueue::Pending&, FakeTransform::Applier& a, Ledger&) { a.world.spatial = false; }},
            {"spatial not distinct", "candidate_spatial_not_distinct", false, [](TransformApply::Context&, CommandQueue::Pending&, FakeTransform::Applier& a, Ledger&) { a.world.spatial_distinct = false; }},
            {"in registry slot 1", "candidate_in_registry", false, [](TransformApply::Context&, CommandQueue::Pending&, FakeTransform::Applier& a, Ledger&) { a.world.in_registry = true; }},
            {"inconclusive populated slot", "candidate_in_registry", false, [](TransformApply::Context&, CommandQueue::Pending&, FakeTransform::Applier& a, Ledger&) { a.world.inconclusive_slot = true; }},
            {"registry unreadable", "registry_unreadable", true, [](TransformApply::Context&, CommandQueue::Pending&, FakeTransform::Applier& a, Ledger&) { a.world.registry_readable = false; }},
            {"hero control unreadable", "hero_control_unavailable", true, [](TransformApply::Context&, CommandQueue::Pending&, FakeTransform::Applier& a, Ledger&) { a.world.hero_readable = false; }},
            {"dirty preimage", "preimage_dirty", false, [](TransformApply::Context&, CommandQueue::Pending&, FakeTransform::Applier& a, Ledger&) { a.world.dirty = true; }},
            {"expect world twin bits", "expect_world_mismatch", false, [](TransformApply::Context&, CommandQueue::Pending& p, FakeTransform::Applier&, Ledger&) { p.command.transform->expect_world.w[11] = 0x3efc305fu; }},
            {"expect local mismatch", "expect_local_mismatch", false, [](TransformApply::Context&, CommandQueue::Pending& p, FakeTransform::Applier&, Ledger&) { p.command.transform->expect_local.w[6] = FloatWords::ToWord(0.9999999f); }},
            {"expect parent mismatch", "expect_parent_mismatch", false, [](TransformApply::Context&, CommandQueue::Pending& p, FakeTransform::Applier&, Ledger&) { p.command.transform->expect_parent = "none"; }},
            {"target equals current (huge z)", "target_equals_current", false, [](TransformApply::Context&, CommandQueue::Pending& p, FakeTransform::Applier& a, Ledger&) { a.world.world = FakeTransform::WorldOf(0.f, 0.f, 3.0e8f); p.command.transform->expect_world = a.world.world; }},
            {"record invalidated", "operation_record_invalidated", false, [](TransformApply::Context&, CommandQueue::Pending&, FakeTransform::Applier&, Ledger& l) { l.Invalidate("test"); }},
            {"restore without a record", "operation_record_missing", false, [](TransformApply::Context&, CommandQueue::Pending& p, FakeTransform::Applier&, Ledger&) { p.command = Restore(p.command.command_id, k_PreWorld, FakeTransform::WorldOf(0.f, 0.f, -2.f)); }},
            {"not playing", "mission_not_playing", false, [](TransformApply::Context& c, CommandQueue::Pending&, FakeTransform::Applier&, Ledger&) { c.mission_playing = false; }},
            {"stable frames", "stable_frames_too_few", false, [](TransformApply::Context& c, CommandQueue::Pending&, FakeTransform::Applier&, Ledger&) { c.rise_frame = 90; }},
            {"contract start missing", "contract_start_missing", false, [](TransformApply::Context& c, CommandQueue::Pending&, FakeTransform::Applier&, Ledger&) { c.contract_started_seen.reset(); }},
            {"fresh generation mismatch", "generation_mismatch_fresh", false, [](TransformApply::Context&, CommandQueue::Pending&, FakeTransform::Applier& a, Ledger&) { a.world.fresh_generation = k_Gen2; }},
            {"fresh generation unavailable", "fresh_generation_unavailable", true, [](TransformApply::Context&, CommandQueue::Pending&, FakeTransform::Applier& a, Ledger&) { a.world.fresh_unavailable = true; }},
            {"epoch changed", "epoch_changed", false, [](TransformApply::Context& c, CommandQueue::Pending&, FakeTransform::Applier&, Ledger&) { c.relay_transition_epoch = 2; }},
            {"scene type", "scene_type_not_mission", false, [](TransformApply::Context& c, CommandQueue::Pending&, FakeTransform::Applier&, Ledger&) { c.scene.scene_type = "menu"; }},
            {"wrong thread", "thread_mismatch", true, [](TransformApply::Context& c, CommandQueue::Pending&, FakeTransform::Applier&, Ledger&) { c.expected_thread_id = c.thread_id + 1; }},
            {"link generation changed", "link_generation_changed", true, [](TransformApply::Context& c, CommandQueue::Pending&, FakeTransform::Applier&, Ledger&) { c.link_generation_now = [] { return uint64_t(2); }; }},
            {"observe faults", "effectful_latched", true, [](TransformApply::Context&, CommandQueue::Pending&, FakeTransform::Applier& a, Ledger&) { a.world.mode = Mode::ObserveFaults; }},
        };

        // M6-G1: the same table for the v1 lift and the v2 step_y (one gate); the z-specific no-op
        // case is the lift's alone (the step's own no-op and non-finite cases are below).
        for (const uint32_t s_Version : {1u, 2u})
        for (const auto& s_Case : s_Cases)
        {
            if (s_Version == 2 && std::string(s_Case.label) == "target equals current (huge z)")
                continue;
            FakeTransform::Applier s_Applier;
            Ledger s_Ledger;
            auto s_Pending = Pending(s_Version == 1 ? Lift(CommandTest::Uuid(20)) : Step(CommandTest::Uuid(20)));
            auto s_Context = PlayingContext(s_Pending);
            s_Case.arrange(s_Context, s_Pending, s_Applier, s_Ledger);
            const auto s_Run = Apply(s_Context, s_Applier, s_Ledger);
            const bool s_Status = s_Run.result.status == (s_Case.unavailable ? Status::Unavailable : Status::PreconditionFailed);
            const bool s_Reason = Contains(s_Run.result.reason, s_Case.reason) && Contains(TextField(s_Run.result, "precondition_reasons"), s_Case.reason);
            if (!s_Status || !s_Reason || s_Applier.invokes != 0)
                std::printf("  transform v%u refusal '%s': status %s reason '%s' invokes %d\n", s_Version, s_Case.label, StatusText(s_Run.result.status), s_Run.result.reason.c_str(), s_Applier.invokes);
            CHECK(s_Status && s_Reason);
            CHECK(s_Applier.invokes == 0 && s_Run.outcome.invocation == Invocation::NotCalled && s_Run.outcome.effect == Effect::None);
            CHECK(!BoolField(s_Run.result, "setter_called", true) && BoolField(s_Run.result, "precondition_ok", false) && TextField(s_Run.result, "apply_log") == "not_attempted");
            CHECK(s_Ledger.forward_budget == 1 || std::string(s_Case.label) == "record invalidated"); // a refusal never consumes the budget
            CHECK(s_Run.result.observed.size() <= k_MaxObservedEntries && EncodeResult(s_Run.result).size() <= k_MaxLineBytes);
            CHECK(FieldIs(s_Run.result, "postimage_world_words", FieldStatus::Unavailable) && FieldIs(s_Run.result, "generation_post", FieldStatus::Unavailable));
        }
    }

    // --- the link generation re-read immediately before the call: refused, nothing invoked ---------
    {
        FakeTransform::Applier s_Applier;
        Ledger s_Ledger;
        const auto s_Pending = Pending(Lift(CommandTest::Uuid(21)));
        auto s_Context = PlayingContext(s_Pending);
        int s_Reads = 0;
        s_Context.link_generation_now = [&] { return ++s_Reads == 1 ? uint64_t(1) : uint64_t(0); };
        const auto s_Run = Apply(s_Context, s_Applier, s_Ledger);
        CHECK(s_Run.result.status == Status::Unavailable && Contains(s_Run.result.reason, "link_generation_changed immediately before the call"));
        CHECK(s_Applier.invokes == 0 && s_Run.outcome.precondition_ok && s_Ledger.forward_budget == 1 && s_Ledger.state == Ledger::State::None);
    }

    // --- the setter's other outcomes, classified as said; never "definitely unapplied" ---------------
    {
        struct Case { Mode mode; Status status; Effect effect; Invocation invocation; const char* world_match; const char* local_match; const char* control; const char* note; };
        const Case s_Cases[] = {
            {Mode::Unchanged, Status::Applied, Effect::Unchanged, Invocation::Called, "preimage", "unchanged", "unchanged", "equal the preimage"},
            {Mode::Elsewhere, Status::Applied, Effect::Uncertain, Invocation::Called, "neither", "unchanged", "unchanged", "match neither"},
            {Mode::AxesElsewhere, Status::Applied, Effect::Uncertain, Invocation::Called, "neither", "unchanged", "unchanged", "match neither"},
            {Mode::LocalFollows, Status::Applied, Effect::Changed, Invocation::Called, "target", "changed", "unchanged", "all twelve world words"},
            {Mode::DirtyPost, Status::Applied, Effect::Uncertain, Invocation::Called, "indeterminate_dirty", "unchanged", "unchanged", "indeterminate"},
            {Mode::ReadBackUnavailable, Status::Applied, Effect::Uncertain, Invocation::Called, "unavailable", "unavailable", "unchanged", "read-back unavailable"},
            {Mode::PostGenerationChange, Status::Applied, Effect::Uncertain, Invocation::Called, "target", "unchanged", "unchanged", "generation changed"},
            {Mode::PostParentChange, Status::Applied, Effect::Uncertain, Invocation::Called, "target", "unchanged", "unchanged", "parent changed"},
            {Mode::PostEntityChange, Status::Applied, Effect::Uncertain, Invocation::Called, "target", "unchanged", "unchanged", "did not resolve the same entity"},
            {Mode::FaultDuringCall, Status::Failed, Effect::Uncertain, Invocation::Faulted, "preimage", "unchanged", "unchanged", "fault inside the setter"}, // the fake's read-back still reads the untouched fake world; the effect stays uncertain
            {Mode::ControlChanges, Status::Applied, Effect::Changed, Invocation::Called, "target", "unchanged", "changed", "hero control changed"},
            {Mode::ControlDrifts, Status::Applied, Effect::Changed, Invocation::Called, "target", "unchanged", "within_tolerance", "hero control within_tolerance"},
        };

        for (const auto& s_Case : s_Cases)
        {
            FakeTransform::Applier s_Applier;
            s_Applier.world.mode = s_Case.mode;
            Ledger s_Ledger;
            const auto s_Pending = Pending(Lift(CommandTest::Uuid(22)));
            const auto s_Run = Apply(PlayingContext(s_Pending), s_Applier, s_Ledger);
            const bool s_Ok = s_Run.result.status == s_Case.status && s_Run.outcome.effect == s_Case.effect && s_Run.outcome.invocation == s_Case.invocation
                && TextField(s_Run.result, "world_match") == s_Case.world_match && TextField(s_Run.result, "local_match") == s_Case.local_match && TextField(s_Run.result, "hero_control") == s_Case.control
                && Contains(s_Run.result.reason, s_Case.note);
            if (!s_Ok)
                std::printf("  transform mode %d: status %s effect %s world_match %s local_match %s control %s reason '%s'\n", static_cast<int>(s_Case.mode), StatusText(s_Run.result.status), OutfitApply::EffectText(s_Run.outcome.effect), TextField(s_Run.result, "world_match").c_str(), TextField(s_Run.result, "local_match").c_str(), TextField(s_Run.result, "hero_control").c_str(), s_Run.result.reason.c_str());
            CHECK(s_Ok);
            CHECK(s_Applier.invokes == 1 && s_Applier.readbacks == 1 && s_Applier.releases == 1);
            CHECK(BoolField(s_Run.result, "setter_called", true) && TextField(s_Run.result, "effect") == OutfitApply::EffectText(s_Case.effect));
            CHECK(TextField(s_Run.result, "preimage_world_words") == FloatWords::Encode(k_PreWorld) && IntField(s_Run.result, "budget_remaining") == 0);
            CHECK(s_Run.result.observed.size() <= k_MaxObservedEntries && EncodeResult(s_Run.result).size() <= k_MaxLineBytes);

            // The ledger: only a clean changed forward with an unchanged control keeps a restorable record.
            const bool s_Restorable = s_Case.effect == Effect::Changed && std::string(s_Case.control) == "unchanged";
            CHECK((s_Ledger.state == Ledger::State::ForwardInvoked) == s_Restorable);
            if (!s_Restorable)
                CHECK(s_Ledger.state == Ledger::State::Invalidated && !s_Ledger.invalidated_reason.empty());

            // A restore after an uncertain, unchanged or control-disturbed forward is refused; nothing is written.
            if (!s_Restorable && s_Case.mode != Mode::FaultDuringCall)
            {
                s_Applier.world.resolved = true; // (ReadBackUnavailable scripted the post read unresolved; the later frame resolves again)
                s_Applier.world.parent = TransformObservation::k_Brick;
                s_Applier.world.post_generation = k_Gen1;
                const Words12 s_Expect = s_Applier.world.world;
                const auto s_RestorePending = Pending(Restore(CommandTest::Uuid(23), s_Expect, k_PreWorld));
                const auto s_Back = Apply(PlayingContext(s_RestorePending), s_Applier, s_Ledger);
                CHECK(s_Back.result.status == Status::PreconditionFailed && Contains(s_Back.result.reason, "operation_record_invalidated") && s_Applier.invokes == 1);
            }

            if (s_Case.mode == Mode::DirtyPost)
                CHECK(BoolField(s_Run.result, "postimage_dirty", true) && TextField(s_Run.result, "postimage_world_words") == FloatWords::Encode(k_Target));
            if (s_Case.mode == Mode::PostGenerationChange)
                CHECK(TextField(s_Run.result, "generation_post") == k_Gen2);
            if (s_Case.mode == Mode::PostEntityChange)
                CHECK(BoolField(s_Run.result, "post_same_entity", false));
            if (s_Case.mode == Mode::FaultDuringCall)
            {
                CHECK(s_Applier.Latched());
                const auto s_Next = Apply(PlayingContext(s_Pending), s_Applier, s_Ledger);
                CHECK(s_Next.result.status == Status::Unavailable && Contains(s_Next.result.reason, "effectful_latched") && s_Applier.invokes == 1);
            }
        }
    }

    // --- the restore's bindings: every substitution refused against the record ----------------------
    {
        struct Case
        {
            const char* label;
            const char* reason;
            void (*arrange)(TransformApply::Context&, CommandQueue::Pending&, FakeTransform::Applier&, Ledger&);
        };
        const Case s_Cases[] = {
            {"wrong original id", "operation_record_mismatch", [](TransformApply::Context&, CommandQueue::Pending& p, FakeTransform::Applier&, Ledger&) { p.command.transform->restore_of = CommandTest::Uuid(77); }},
            {"substituted matrix", "restore_target_mismatch", [](TransformApply::Context&, CommandQueue::Pending& p, FakeTransform::Applier&, Ledger&) { p.command.transform->target_world = FakeTransform::WorldOf(0.f, 0.f, -2.f); }},
            {"reconnect (link changed)", "restore_link_changed", [](TransformApply::Context& c, CommandQueue::Pending& p, FakeTransform::Applier&, Ledger&) { c.link_generation = 2; c.link_generation_now = [] { return uint64_t(2); }; p.link_generation = 2; }},
            {"mission generation changed", "restore_generation_changed", [](TransformApply::Context& c, CommandQueue::Pending& p, FakeTransform::Applier& a, Ledger&) { a.world.fresh_generation = k_Gen2; c.mission_generation = k_Gen2; c.contract_started_seen = k_Gen2; p.at_receipt.mission_generation = k_Gen2; p.command.mission_generation_precondition = k_Gen2; }},
            {"state drifted since the lift", "current_not_postimage", [](TransformApply::Context&, CommandQueue::Pending& p, FakeTransform::Applier& a, Ledger&) { a.world.world.w[11] = FloatWords::ToWord(FloatWords::ToFloat(a.world.world.w[11]) + 0.5f); p.command.transform->expect_world = a.world.world; }},
            {"stale expectation (pre-lift words)", "expect_world_mismatch", [](TransformApply::Context&, CommandQueue::Pending& p, FakeTransform::Applier&, Ledger&) { p.command.transform->expect_world = k_PreWorld; p.command.transform->target_world = FakeTransform::WorldOf(0.f, 0.f, -2.f); }},
            {"lost record (new process)", "operation_record_missing", [](TransformApply::Context&, CommandQueue::Pending&, FakeTransform::Applier&, Ledger& l) { l = Ledger{}; }},
        };

        for (const auto& s_Case : s_Cases)
        {
            FakeTransform::Applier s_Applier;
            Ledger s_Ledger;
            const auto s_Lifted = LiftOnce(s_Applier, s_Ledger);
            CHECK(s_Lifted.outcome.effect == Effect::Changed);
            auto s_Pending = Pending(Restore(CommandTest::Uuid(24), k_Target, k_PreWorld));
            auto s_Context = PlayingContext(s_Pending);
            s_Case.arrange(s_Context, s_Pending, s_Applier, s_Ledger);
            const auto s_Run = Apply(s_Context, s_Applier, s_Ledger);
            const bool s_Ok = s_Run.result.status == Status::PreconditionFailed && Contains(s_Run.result.reason, s_Case.reason) && s_Applier.invokes == 1;
            if (!s_Ok)
                std::printf("  restore refusal '%s': status %s reason '%s' invokes %d\n", s_Case.label, StatusText(s_Run.result.status), s_Run.result.reason.c_str(), s_Applier.invokes);
            CHECK(s_Ok);
            CHECK(s_Run.outcome.invocation == Invocation::NotCalled && TextField(s_Run.result, "apply_log") == "not_attempted");
        }

        // An engine return to baseline before the restore: current_not_postimage (and the planner
        // on the BEAM side records that no restore is needed); nothing is written.
        {
            FakeTransform::Applier s_Applier;
            s_Applier.world.later = Later::Returned;
            Ledger s_Ledger;
            LiftOnce(s_Applier, s_Ledger);
            const auto s_Later = s_Applier.Observe(); // the scripted later frame: the preimage is back
            CHECK(s_Later.candidate.world.observed() && *s_Later.candidate.world.value == k_PreWorld);
            auto s_Pending = Pending(Restore(CommandTest::Uuid(25), k_PreWorld, k_PreWorld));
            const auto s_Run = Apply(PlayingContext(s_Pending), s_Applier, s_Ledger);
            CHECK(s_Run.result.status == Status::PreconditionFailed && Contains(s_Run.result.reason, "target_equals_current") && Contains(s_Run.result.reason, "current_not_postimage") && s_Applier.invokes == 1);
        }

        // The restore's own read-back outcomes: a restore that did not reach the preimage reports
        // its facts honestly and the ledger closes the record either way.
        {
            FakeTransform::Applier s_Applier;
            Ledger s_Ledger;
            LiftOnce(s_Applier, s_Ledger);
            s_Applier.world.mode = Mode::Elsewhere;
            const auto s_Pending = Pending(Restore(CommandTest::Uuid(26), k_Target, k_PreWorld));
            const auto s_Run = Apply(PlayingContext(s_Pending), s_Applier, s_Ledger);
            CHECK(s_Run.result.status == Status::Applied && s_Run.outcome.effect == Effect::Uncertain && TextField(s_Run.result, "world_match") == "neither");
            CHECK(TextField(s_Run.result, "restore_facts") == "world=0,local=1,parent=1,dirty=1,snapshot=0,within_tol=0");
            CHECK(s_Ledger.state == Ledger::State::Restored && s_Applier.invokes == 2);
        }
    }

    // --- the checked "applying" line: no call on an unaccepted line; the budget is not consumed -----
    {
        struct Case { const char* label; bool file_open; bool succeeded; uint32_t short_by; uint32_t error; const char* status; };
        const Case s_Cases[] = {
            {"file unavailable", false, false, 0, 3, "file_unavailable"},
            {"write failed", true, false, 0, 29, "write_failed"},
            {"short write", true, true, 7, 0, "short_write"},
        };

        for (const auto& s_Case : s_Cases)
        {
            FakeTransform::Applier s_Applier;
            Ledger s_Ledger;
            ResetLogScript(&s_Applier);
            g_LogScript.file_open = s_Case.file_open;
            g_LogScript.succeeded = s_Case.succeeded;
            g_LogScript.short_by = s_Case.short_by;
            g_LogScript.error = s_Case.error;
            const auto s_Previous = RelayLog::SetRawWriterForTests(&ScriptedRawWrite);
            const auto s_Pending = Pending(Lift(CommandTest::Uuid(27)));
            const auto s_Run = Apply(PlayingContext(s_Pending), s_Applier, s_Ledger);
            RelayLog::SetRawWriterForTests(s_Previous);
            const bool s_Ok = s_Run.result.status == Status::Unavailable && Contains(s_Run.result.reason, "apply_log_unwritten") && Contains(s_Run.result.reason, s_Case.status) && s_Applier.invokes == 0;
            if (!s_Ok)
                std::printf("  transform log case '%s': status %s reason '%s' invokes %d\n", s_Case.label, StatusText(s_Run.result.status), s_Run.result.reason.c_str(), s_Applier.invokes);
            CHECK(s_Ok);
            CHECK(TextField(s_Run.result, "apply_log") == s_Case.status && !BoolField(s_Run.result, "setter_called", true) && s_Run.outcome.precondition_ok);
            CHECK(g_LogWrites >= 1 && Contains(g_LogLines.back(), "applying candidate.transform.set/1 mode lift") && Contains(g_LogLines.back(), FloatWords::Encode(k_PreWorld)) && Contains(g_LogLines.back(), FloatWords::Encode(k_Target)));
            CHECK(s_Ledger.forward_budget == 1 && s_Ledger.state == Ledger::State::None);

            // The next lift, with the writer restored, succeeds: the refusal did not latch or consume.
            const auto s_Retry = Apply(PlayingContext(Pending(Lift(CommandTest::Uuid(28)))), s_Applier, s_Ledger);
            CHECK(s_Retry.result.status == Status::Applied && s_Retry.outcome.effect == Effect::Changed && s_Applier.invokes == 1 && s_Ledger.forward_budget == 0);
        }

        // The accepted line precedes the call: the write is recorded before the invoke count moves.
        {
            FakeTransform::Applier s_Applier;
            Ledger s_Ledger;
            ResetLogScript(&s_Applier);
            const auto s_Previous = RelayLog::SetRawWriterForTests(&ScriptedRawWrite);
            const auto s_Run = LiftOnce(s_Applier, s_Ledger, CommandTest::Uuid(29));
            RelayLog::SetRawWriterForTests(s_Previous);
            CHECK(s_Run.result.status == Status::Applied && g_InvokesAtLastWrite == 0 && s_Applier.invokes == 1);
            CHECK(Contains(g_LogLines.back(), "candidate feede715906f747f in 007c57a6ea581f8c") && Contains(g_LogLines.back(), "dirty 0") && Contains(g_LogLines.back(), "3efc305e") && Contains(g_LogLines.back(), "3f3e182f"));
        }
    }

    // --- the mandatory result's capacity on the actual encoding ----------------------------------
    {
        FakeTransform::Applier s_Applier;
        s_Applier.world.fresh_generation = std::string(k_MaxMissionGenerationBytes, 'g');
        s_Applier.world.parent = "ffffffffffffffff";
        s_Applier.world.in_registry = false;
        s_Applier.world.inconclusive_slot = true; // the longest slot text
        const auto s_Reads = s_Applier.Current(false, s_Applier.world.fresh_generation);
        Result s_Base;
        s_Base.protocol_version = 2;
        s_Base.command_id = CommandTest::Uuid(30);
        s_Base.link_generation = UINT64_MAX;
        s_Base.adapter_instance_id = CommandTest::Uuid(99);
        s_Base.native_at = std::string(64, 't');
        s_Base.frame = UINT64_MAX;
        s_Base.mission_generation = s_Applier.world.fresh_generation;
        TransformRequest s_Request;
        s_Request.mode = TransformMode::Restore;
        s_Request.restore_of = CommandTest::Uuid(31);
        CHECK(TransformApply::MandatoryResultFits(s_Base, s_Reads, s_Request, k_Target));
        s_Request.mode = TransformMode::Lift;
        s_Request.restore_of.reset();
        CHECK(TransformApply::MandatoryResultFits(s_Base, s_Reads, s_Request, k_Target));
    }

    // --- the scheduler's dispatch: rejected without an applier; applied with one; the ledger kept ---
    {
        CommandQueue s_Queue;
        std::vector<Result> s_Emitted;
        CommandScheduler s_Scheduler(s_Queue, [&](const Result& p_Result) { s_Emitted.push_back(p_Result); });
        FakeTransform::Applier s_Applier;

        CommandScheduler::FrameObservation s_Frame;
        s_Frame.frame = 100;
        s_Frame.now_mono_ms = 5000;
        s_Frame.thread_id = GetCurrentThreadId();
        s_Frame.link_generation = 1;
        s_Frame.link_generation_now = [] { return uint64_t(1); };
        s_Frame.scene_available = true;
        s_Frame.scene.available = true;
        s_Frame.scene.scene_resource = "r";
        s_Frame.scene.scene_loaded = true;
        s_Frame.scene.loading_stage = 8;
        s_Frame.scene.scene_type = "mission";
        s_Frame.mission_playing = true;
        s_Frame.previous_frame_playing = true;
        s_Frame.mission_generation = k_Gen1;
        s_Frame.relay_transition_epoch = 1;
        s_Frame.rise_frame = 1;
        s_Frame.rise_mono_ms = 0;
        s_Frame.scene_resource_same_as_rise = true;
        s_Frame.contract_started_seen = k_Gen1;

        // Without an applier (the inert frame source): rejected, nothing applied, ledger untouched.
        CHECK(s_Queue.Push(Lift(CommandTest::Uuid(40)), 1, 4900).outcome == CommandQueue::PushOutcome::Queued);
        CHECK(s_Scheduler.Tick(s_Frame) && s_Emitted.size() == 1 && s_Emitted[0].status == Status::Rejected && Contains(s_Emitted[0].reason, "no transform applier"));
        CHECK(s_Scheduler.GetStats().transform_dispatched == 1 && s_Scheduler.GetStats().rejected == 1 && s_Scheduler.TransformLedger().forward_budget == 1);

        // With one: applied, changed; the stats and the ledger move.
        s_Frame.transform_applier = &s_Applier;
        CHECK(s_Queue.Push(Lift(CommandTest::Uuid(41)), 1, 4900).outcome == CommandQueue::PushOutcome::Queued);
        CHECK(s_Scheduler.Tick(s_Frame) && s_Emitted.size() == 2 && s_Emitted[1].status == Status::Applied && TextField(s_Emitted[1], "effect") == "changed");
        CHECK(s_Scheduler.GetStats().transform_called == 1 && s_Scheduler.GetStats().transform_changed == 1 && s_Scheduler.TransformLedger().state == Ledger::State::ForwardInvoked && s_Scheduler.TransformLedger().forward_id == CommandTest::Uuid(41));

        // A duplicate id is refused by the queue; a fresh id is refused by the budget.
        CHECK(s_Queue.Push(Lift(CommandTest::Uuid(41)), 1, 4900).outcome == CommandQueue::PushOutcome::DuplicateDone);
        CHECK(s_Queue.Push(Lift(CommandTest::Uuid(42), k_Target), 1, 4900).outcome == CommandQueue::PushOutcome::Queued);
        CHECK(s_Scheduler.Tick(s_Frame) && s_Emitted.size() == 3 && s_Emitted[2].status == Status::PreconditionFailed && Contains(s_Emitted[2].reason, "budget_exhausted"));
        CHECK(s_Scheduler.GetStats().transform_not_applied == 1 && s_Applier.invokes == 1);

        // An expired TTL never reaches the apply.
        Command s_Late = Lift(CommandTest::Uuid(43), k_Target);
        s_Late.ttl_ms = k_MinTtlMs;
        CHECK(s_Queue.Push(s_Late, 1, 4000).outcome == CommandQueue::PushOutcome::Queued);
        CHECK(s_Scheduler.Tick(s_Frame) && s_Emitted.size() == 4 && s_Emitted[3].status == Status::Expired && s_Applier.invokes == 1);
    }
    // ===== M6-G1: candidate.transform.set v2 step_y over the same gate, applier and ledger ==========
    const Words12 k_E3World = W12("3f800000,00000000,00000000,00000000,3f800000,00000000,00000000,00000000,3f800000,c2b2fae1,416d47ae,bfc2d6ba");
    const Words7 k_E3Local = W7("c2b2fae1,416d47ae,bfc2d6ba,80000000,00000000,80000000,3f800000");
    const Words12 k_StepTarget = TransformObservation::StepYTarget(k_E3World);
    auto AtP = [&](FakeTransform::Applier& p_Applier) { p_Applier.world.world = k_E3World; p_Applier.world.local = k_E3Local; };
    const std::string k_StepId = "5b1f0c2e-8d4a-4f6b-9a3c-2e7d1f0a9b84";

    // --- the qualified step on M6-E3's words; the shared budget; a v1 restore refused; the v2 restore ---
    {
        FakeTransform::Applier s_Applier;
        AtP(s_Applier);
        Ledger s_Ledger;
        ResetLogScript(&s_Applier);
        const auto s_Previous = RelayLog::SetRawWriterForTests(&ScriptedRawWrite);
        const auto s_Run = Apply(PlayingContext(Pending(Step(k_StepId, k_E3World, k_E3Local))), s_Applier, s_Ledger);
        RelayLog::SetRawWriterForTests(s_Previous);
        CHECK(s_Run.result.status == Status::Applied && s_Run.outcome.invocation == Invocation::Called && s_Run.outcome.effect == Effect::Changed);
        CHECK(s_Applier.invokes == 1 && s_Applier.last_target == k_StepTarget);
        for (size_t i = 0; i < 12; ++i)
            CHECK(i == 10 ? s_Applier.last_target.w[i] == 0x417147aeu : s_Applier.last_target.w[i] == k_E3World.w[i]);
        CHECK(TextField(s_Run.result, "mode") == "step_y" && TextField(s_Run.result, "requested_world_words") == FloatWords::Encode(k_StepTarget));
        CHECK(TextField(s_Run.result, "preimage_world_words") == FloatWords::Encode(k_E3World) && TextField(s_Run.result, "preimage_local_words") == FloatWords::Encode(k_E3Local));
        CHECK(TextField(s_Run.result, "requested_world") == "-89.49,15.08,-1.522178");
        CHECK(TextField(s_Run.result, "world_match") == "target" && TextField(s_Run.result, "local_match") == "unchanged" && TextField(s_Run.result, "hero_control") == "unchanged");
        CHECK(IntField(s_Run.result, "budget_remaining") == 0 && TextField(s_Run.result, "apply_log") == "written" && BoolField(s_Run.result, "setter_called", true));
        CHECK(s_Run.result.observed.size() == 47 && EncodeResult(s_Run.result).size() <= k_MaxLineBytes && !s_Run.outcome.reduced);
        std::printf("line bound: candidate.transform.set/2 step_y result = %zu bytes of %zu (%zu entries)\n", EncodeResult(s_Run.result).size(), k_MaxLineBytes, s_Run.result.observed.size());
        CHECK(!g_LogLines.empty() && Contains(g_LogLines.back(), "applying candidate.transform.set/2 mode step_y") && Contains(g_LogLines.back(), FloatWords::Encode(k_E3World)) && Contains(g_LogLines.back(), FloatWords::Encode(k_StepTarget)));
        CHECK(g_InvokesAtLastWrite == 0); // the accepted line precedes the call
        CHECK(s_Ledger.state == Ledger::State::ForwardInvoked && s_Ledger.forward_budget == 0 && s_Ledger.forward_id == k_StepId);
        CHECK(s_Ledger.forward_version == 2 && s_Ledger.forward_mode == TransformMode::StepY && s_Ledger.preimage_world == k_E3World && s_Ledger.requested_world == k_StepTarget && s_Ledger.postimage_world == k_StepTarget);

        // One shared budget: a v1 lift after the step, and a second step, are refused; nothing invoked.
        const auto s_LiftAfter = Apply(PlayingContext(Pending(Lift(CommandTest::Uuid(60), k_StepTarget, k_E3Local))), s_Applier, s_Ledger);
        CHECK(s_LiftAfter.result.status == Status::PreconditionFailed && Contains(s_LiftAfter.result.reason, "budget_exhausted") && s_Applier.invokes == 1);
        const auto s_StepAgain = Apply(PlayingContext(Pending(Step(CommandTest::Uuid(61), k_StepTarget, k_E3Local))), s_Applier, s_Ledger);
        CHECK(s_StepAgain.result.status == Status::PreconditionFailed && Contains(s_StepAgain.result.reason, "budget_exhausted") && s_Applier.invokes == 1);

        // A v1 restore of the v2 step: refused natively (restore_version_mismatch); nothing invoked.
        const auto s_CrossRestore = Apply(PlayingContext(Pending(Restore(CommandTest::Uuid(62), k_StepTarget, k_E3World, k_StepId, k_E3Local))), s_Applier, s_Ledger);
        CHECK(s_CrossRestore.result.status == Status::PreconditionFailed && Contains(s_CrossRestore.result.reason, "restore_version_mismatch") && s_Applier.invokes == 1);
        CHECK(s_Ledger.state == Ledger::State::ForwardInvoked);

        // The v2 restore bound to the record: invoked once more, back to M6-E3's words exactly.
        const auto s_Back = Apply(PlayingContext(Pending(RestoreV2(CommandTest::Uuid(63), k_StepTarget, k_E3World, k_StepId, k_E3Local))), s_Applier, s_Ledger);
        CHECK(s_Back.result.status == Status::Applied && s_Back.outcome.effect == Effect::Changed && s_Applier.invokes == 2 && s_Applier.last_target == k_E3World);
        CHECK(TextField(s_Back.result, "mode") == "restore" && TextField(s_Back.result, "restore_of") == k_StepId);
        CHECK(TextField(s_Back.result, "restore_facts") == "world=1,local=1,parent=1,dirty=1,snapshot=1,within_tol=1" && s_Ledger.state == Ledger::State::Restored);

        // A restoration never refills the budget: a further step and a further lift are refused.
        const auto s_StepAfterRestore = Apply(PlayingContext(Pending(Step(CommandTest::Uuid(64), k_E3World, k_E3Local))), s_Applier, s_Ledger);
        CHECK(s_StepAfterRestore.result.status == Status::PreconditionFailed && Contains(s_StepAfterRestore.result.reason, "budget_exhausted") && s_Applier.invokes == 2);
        const auto s_LiftAfterRestore = Apply(PlayingContext(Pending(Lift(CommandTest::Uuid(65), k_E3World, k_E3Local))), s_Applier, s_Ledger);
        CHECK(s_LiftAfterRestore.result.status == Status::PreconditionFailed && Contains(s_LiftAfterRestore.result.reason, "budget_exhausted") && s_Applier.invokes == 2);
        const auto s_SecondRestore = Apply(PlayingContext(Pending(RestoreV2(CommandTest::Uuid(66), k_E3World, k_StepTarget, k_StepId, k_E3Local))), s_Applier, s_Ledger);
        CHECK(s_SecondRestore.result.status == Status::PreconditionFailed && Contains(s_SecondRestore.result.reason, "already_restored") && s_Applier.invokes == 2);
    }

    // --- the other order: a v1 lift first; a v2 step refused (shared budget); a v2 restore of the lift refused ---
    {
        FakeTransform::Applier s_Applier;
        Ledger s_Ledger;
        const auto s_Lifted = LiftOnce(s_Applier, s_Ledger);
        CHECK(s_Lifted.outcome.effect == Effect::Changed && s_Ledger.forward_version == 1 && s_Ledger.forward_mode == TransformMode::Lift);
        const auto s_StepAfter = Apply(PlayingContext(Pending(Step(CommandTest::Uuid(67), k_Target))), s_Applier, s_Ledger);
        CHECK(s_StepAfter.result.status == Status::PreconditionFailed && Contains(s_StepAfter.result.reason, "budget_exhausted") && s_Applier.invokes == 1);
        const auto s_CrossRestore = Apply(PlayingContext(Pending(RestoreV2(CommandTest::Uuid(68), k_Target, k_PreWorld, k_Original, k_PreLocal))), s_Applier, s_Ledger);
        CHECK(s_CrossRestore.result.status == Status::PreconditionFailed && Contains(s_CrossRestore.result.reason, "restore_version_mismatch") && s_Applier.invokes == 1);
        // the v1 restore of the v1 lift still works exactly as before
        const auto s_Back = Apply(PlayingContext(Pending(Restore(CommandTest::Uuid(69), k_Target, k_PreWorld))), s_Applier, s_Ledger);
        CHECK(s_Back.result.status == Status::Applied && s_Back.outcome.effect == Effect::Changed && s_Applier.invokes == 2 && s_Ledger.state == Ledger::State::Restored);
    }

    // --- unsupported (version, mode) pairs reached natively (BEAM and the decoder refuse them first) ---
    {
        struct Pair { uint32_t version; TransformMode mode; };
        const Pair s_Pairs[] = {{1, TransformMode::StepY}, {2, TransformMode::Lift}, {3, TransformMode::StepY}, {0, TransformMode::Lift}};
        for (const auto& s_Pair : s_Pairs)
        {
            FakeTransform::Applier s_Applier;
            Ledger s_Ledger;
            Command s_Command = Lift(CommandTest::Uuid(70));
            s_Command.transform->version = s_Pair.version;
            s_Command.transform->mode = s_Pair.mode;
            const auto s_Run = Apply(PlayingContext(Pending(s_Command)), s_Applier, s_Ledger);
            const bool s_Ok = s_Run.result.status == Status::PreconditionFailed && Contains(s_Run.result.reason, "entry_mode_unsupported") && s_Applier.invokes == 0;
            if (!s_Ok)
                std::printf("  unsupported pair v%u %s: status %s reason '%s' invokes %d\n", s_Pair.version, TransformModeText(s_Pair.mode), StatusText(s_Run.result.status), s_Run.result.reason.c_str(), s_Applier.invokes);
            CHECK(s_Ok);
            CHECK(s_Ledger.forward_budget == 1 && s_Ledger.state == Ledger::State::None && FieldIs(s_Run.result, "requested_world_words", FieldStatus::Unavailable));
        }
        // the request's version disagreeing with the decoded command's entry version: refused, nothing invoked
        {
            FakeTransform::Applier s_Applier;
            Ledger s_Ledger;
            Command s_Mixed = Step(CommandTest::Uuid(79));
            s_Mixed.version = 1; // the command says v1, the request says v2 step_y
            const auto s_Run = Apply(PlayingContext(Pending(s_Mixed)), s_Applier, s_Ledger);
            CHECK(s_Run.result.status == Status::PreconditionFailed && Contains(s_Run.result.reason, "entry_mode_unsupported") && s_Applier.invokes == 0 && s_Ledger.forward_budget == 1);
        }
        // a restore whose request names another version than the decoded one never reaches the record either
        FakeTransform::Applier s_Applier;
        Ledger s_Ledger;
        LiftOnce(s_Applier, s_Ledger);
        Command s_Restore = Restore(CommandTest::Uuid(71), k_Target, k_PreWorld);
        s_Restore.transform->version = 3;
        const auto s_Run = Apply(PlayingContext(Pending(s_Restore)), s_Applier, s_Ledger);
        CHECK(s_Run.result.status == Status::PreconditionFailed && Contains(s_Run.result.reason, "entry_mode_unsupported") && Contains(s_Run.result.reason, "restore_version_mismatch") && s_Applier.invokes == 1);
    }

    // --- the checked applying line for /2: no call on an unaccepted line; the budget is not consumed ---
    {
        struct Case { const char* label; bool file_open; bool succeeded; uint32_t short_by; uint32_t error; const char* status; };
        const Case s_Cases[] = {
            {"file unavailable", false, false, 0, 3, "file_unavailable"},
            {"write failed", true, false, 0, 29, "write_failed"},
            {"short write", true, true, 7, 0, "short_write"},
        };
        for (const auto& s_Case : s_Cases)
        {
            FakeTransform::Applier s_Applier;
            AtP(s_Applier);
            Ledger s_Ledger;
            ResetLogScript(&s_Applier);
            g_LogScript.file_open = s_Case.file_open;
            g_LogScript.succeeded = s_Case.succeeded;
            g_LogScript.short_by = s_Case.short_by;
            g_LogScript.error = s_Case.error;
            const auto s_Previous = RelayLog::SetRawWriterForTests(&ScriptedRawWrite);
            const auto s_Run = Apply(PlayingContext(Pending(Step(CommandTest::Uuid(72), k_E3World, k_E3Local))), s_Applier, s_Ledger);
            RelayLog::SetRawWriterForTests(s_Previous);
            CHECK(s_Run.result.status == Status::Unavailable && Contains(s_Run.result.reason, "apply_log_unwritten") && Contains(s_Run.result.reason, s_Case.status) && s_Applier.invokes == 0);
            CHECK(g_LogWrites >= 1 && Contains(g_LogLines.back(), "applying candidate.transform.set/2 mode step_y"));
            CHECK(s_Ledger.forward_budget == 1 && s_Ledger.state == Ledger::State::None);
            const auto s_Retry = Apply(PlayingContext(Pending(Step(CommandTest::Uuid(73), k_E3World, k_E3Local))), s_Applier, s_Ledger);
            CHECK(s_Retry.result.status == Status::Applied && s_Retry.outcome.effect == Effect::Changed && s_Applier.invokes == 1 && s_Ledger.forward_budget == 0);
        }
    }

    // --- the step's other setter outcomes: the same classes as the lift's ------------------------
    {
        struct Case { Mode mode; Status status; Effect effect; const char* world_match; const char* local_match; const char* control; };
        const Case s_Cases[] = {
            {Mode::Unchanged, Status::Applied, Effect::Unchanged, "preimage", "unchanged", "unchanged"},
            {Mode::Elsewhere, Status::Applied, Effect::Uncertain, "neither", "unchanged", "unchanged"},
            {Mode::AxesElsewhere, Status::Applied, Effect::Uncertain, "neither", "unchanged", "unchanged"},
            {Mode::LocalFollows, Status::Applied, Effect::Changed, "target", "changed", "unchanged"},
            {Mode::DirtyPost, Status::Applied, Effect::Uncertain, "indeterminate_dirty", "unchanged", "unchanged"},
            {Mode::ReadBackUnavailable, Status::Applied, Effect::Uncertain, "unavailable", "unavailable", "unchanged"},
            {Mode::PostGenerationChange, Status::Applied, Effect::Uncertain, "target", "unchanged", "unchanged"},
            {Mode::PostParentChange, Status::Applied, Effect::Uncertain, "target", "unchanged", "unchanged"},
            {Mode::PostEntityChange, Status::Applied, Effect::Uncertain, "target", "unchanged", "unchanged"},
            {Mode::FaultDuringCall, Status::Failed, Effect::Uncertain, "preimage", "unchanged", "unchanged"},
            {Mode::ControlChanges, Status::Applied, Effect::Changed, "target", "unchanged", "changed"},
        };
        for (const auto& s_Case : s_Cases)
        {
            FakeTransform::Applier s_Applier;
            AtP(s_Applier);
            s_Applier.world.mode = s_Case.mode;
            Ledger s_Ledger;
            const auto s_Run = Apply(PlayingContext(Pending(Step(CommandTest::Uuid(74), k_E3World, k_E3Local))), s_Applier, s_Ledger);
            const bool s_Ok = s_Run.result.status == s_Case.status && s_Run.outcome.effect == s_Case.effect && TextField(s_Run.result, "world_match") == s_Case.world_match
                && TextField(s_Run.result, "local_match") == s_Case.local_match && TextField(s_Run.result, "hero_control") == s_Case.control;
            if (!s_Ok)
                std::printf("  step mode %d: status %s effect %s world_match %s local_match %s control %s reason '%s'\n", static_cast<int>(s_Case.mode), StatusText(s_Run.result.status), OutfitApply::EffectText(s_Run.outcome.effect), TextField(s_Run.result, "world_match").c_str(), TextField(s_Run.result, "local_match").c_str(), TextField(s_Run.result, "hero_control").c_str(), s_Run.result.reason.c_str());
            CHECK(s_Ok && s_Applier.invokes == 1 && IntField(s_Run.result, "budget_remaining") == 0);
            const bool s_Restorable = s_Case.effect == Effect::Changed && std::string(s_Case.control) == "unchanged";
            CHECK((s_Ledger.state == Ledger::State::ForwardInvoked) == s_Restorable && s_Ledger.forward_version == 2);
            if (s_Case.mode == Mode::LocalFollows)
                CHECK(s_Applier.world.local.w[1] == 0x417147aeu && s_Applier.world.local.w[3] == 0x80000000u && s_Applier.world.local.w[5] == 0x80000000u); // y followed; the signed zeros kept
        }
    }

    // --- the step's own no-op and non-finite targets: refused by the existing rules -------------
    {
        FakeTransform::Applier s_Applier;
        s_Applier.world.world = FakeTransform::WorldOf(0.f, 3.0e8f, -1.522178f);
        Ledger s_Ledger;
        const auto s_Run = Apply(PlayingContext(Pending(Step(CommandTest::Uuid(75), s_Applier.world.world))), s_Applier, s_Ledger);
        CHECK(s_Run.result.status == Status::PreconditionFailed && Contains(s_Run.result.reason, "target_equals_current") && s_Applier.invokes == 0 && s_Ledger.forward_budget == 1);
        FakeTransform::Applier s_Inf;
        s_Inf.world.world.w[10] = 0x7f800000u;
        Ledger s_InfLedger;
        const auto s_InfRun = Apply(PlayingContext(Pending(Step(CommandTest::Uuid(76), s_Inf.world.world))), s_Inf, s_InfLedger);
        CHECK(s_InfRun.result.status == Status::PreconditionFailed && Contains(s_InfRun.result.reason, "target_not_finite") && s_Inf.invokes == 0 && s_InfLedger.forward_budget == 1);
    }

    // --- the v2 restore's bindings: every substitution refused against the record ----------------
    {
        struct Case { const char* label; const char* reason; void (*arrange)(CommandQueue::Pending&, FakeTransform::Applier&, Ledger&); };
        const Case s_Cases[] = {
            {"wrong original id", "operation_record_mismatch", [](CommandQueue::Pending& p, FakeTransform::Applier&, Ledger&) { p.command.transform->restore_of = CommandTest::Uuid(77); }},
            {"substituted matrix", "restore_target_mismatch", [](CommandQueue::Pending& p, FakeTransform::Applier&, Ledger&) { p.command.transform->target_world = FakeTransform::WorldOf(0.f, 0.f, -2.f); }},
            {"state drifted since the step", "current_not_postimage", [](CommandQueue::Pending& p, FakeTransform::Applier& a, Ledger&) { a.world.world.w[10] = FloatWords::ToWord(FloatWords::ToFloat(a.world.world.w[10]) + 0.5f); p.command.transform->expect_world = a.world.world; }},
            {"lost record (new process)", "operation_record_missing", [](CommandQueue::Pending&, FakeTransform::Applier&, Ledger& l) { l = Ledger{}; }},
        };
        for (const auto& s_Case : s_Cases)
        {
            FakeTransform::Applier s_Applier;
            AtP(s_Applier);
            Ledger s_Ledger;
            const auto s_Stepped = Apply(PlayingContext(Pending(Step(k_StepId, k_E3World, k_E3Local))), s_Applier, s_Ledger);
            CHECK(s_Stepped.outcome.effect == Effect::Changed);
            auto s_Pending = Pending(RestoreV2(CommandTest::Uuid(78), k_StepTarget, k_E3World, k_StepId, k_E3Local));
            s_Case.arrange(s_Pending, s_Applier, s_Ledger);
            const auto s_Run = Apply(PlayingContext(s_Pending), s_Applier, s_Ledger);
            const bool s_Ok = s_Run.result.status == Status::PreconditionFailed && Contains(s_Run.result.reason, s_Case.reason) && s_Applier.invokes == 1;
            if (!s_Ok)
                std::printf("  v2 restore refusal '%s': status %s reason '%s' invokes %d\n", s_Case.label, StatusText(s_Run.result.status), s_Run.result.reason.c_str(), s_Applier.invokes);
            CHECK(s_Ok);
        }
    }

    // --- the step's mandatory result capacity; the scheduler's v2 dispatch and the shared budget ---
    {
        FakeTransform::Applier s_Applier;
        s_Applier.world.fresh_generation = std::string(k_MaxMissionGenerationBytes, 'g');
        s_Applier.world.parent = "ffffffffffffffff";
        s_Applier.world.inconclusive_slot = true;
        const auto s_Reads = s_Applier.Current(false, s_Applier.world.fresh_generation);
        Result s_Base;
        s_Base.protocol_version = 2;
        s_Base.command_id = CommandTest::Uuid(80);
        s_Base.link_generation = UINT64_MAX;
        s_Base.adapter_instance_id = CommandTest::Uuid(99);
        s_Base.native_at = std::string(64, 't');
        s_Base.frame = UINT64_MAX;
        s_Base.mission_generation = s_Applier.world.fresh_generation;
        TransformRequest s_Request;
        s_Request.version = 2;
        s_Request.mode = TransformMode::StepY;
        CHECK(TransformApply::MandatoryResultFits(s_Base, s_Reads, s_Request, k_StepTarget));
    }
    {
        CommandQueue s_Queue;
        std::vector<Result> s_Emitted;
        CommandScheduler s_Scheduler(s_Queue, [&](const Result& p_Result) { s_Emitted.push_back(p_Result); });
        FakeTransform::Applier s_Applier;
        CommandScheduler::FrameObservation s_Frame;
        s_Frame.frame = 100;
        s_Frame.now_mono_ms = 5000;
        s_Frame.thread_id = GetCurrentThreadId();
        s_Frame.link_generation = 1;
        s_Frame.link_generation_now = [] { return uint64_t(1); };
        s_Frame.scene_available = true;
        s_Frame.scene.available = true;
        s_Frame.scene.scene_resource = "r";
        s_Frame.scene.scene_loaded = true;
        s_Frame.scene.loading_stage = 8;
        s_Frame.scene.scene_type = "mission";
        s_Frame.mission_playing = true;
        s_Frame.previous_frame_playing = true;
        s_Frame.mission_generation = k_Gen1;
        s_Frame.relay_transition_epoch = 1;
        s_Frame.rise_frame = 1;
        s_Frame.rise_mono_ms = 0;
        s_Frame.scene_resource_same_as_rise = true;
        s_Frame.contract_started_seen = k_Gen1;
        CHECK(s_Queue.Push(Step(CommandTest::Uuid(81)), 1, 4900).outcome == CommandQueue::PushOutcome::Queued);
        CHECK(s_Scheduler.Tick(s_Frame) && s_Emitted.size() == 1 && s_Emitted[0].status == Status::Rejected && Contains(s_Emitted[0].reason, "no transform applier"));
        s_Frame.transform_applier = &s_Applier;
        CHECK(s_Queue.Push(Step(CommandTest::Uuid(82)), 1, 4900).outcome == CommandQueue::PushOutcome::Queued);
        CHECK(s_Scheduler.Tick(s_Frame) && s_Emitted.size() == 2 && s_Emitted[1].status == Status::Applied && TextField(s_Emitted[1], "mode") == "step_y" && TextField(s_Emitted[1], "effect") == "changed");
        CHECK(s_Scheduler.TransformLedger().forward_version == 2 && s_Scheduler.TransformLedger().forward_id == CommandTest::Uuid(82) && s_Scheduler.GetStats().transform_called == 1);
        CHECK(s_Queue.Push(Lift(CommandTest::Uuid(83), TransformObservation::StepYTarget(k_PreWorld)), 1, 4900).outcome == CommandQueue::PushOutcome::Queued);
        CHECK(s_Scheduler.Tick(s_Frame) && s_Emitted.size() == 3 && s_Emitted[2].status == Status::PreconditionFailed && Contains(s_Emitted[2].reason, "budget_exhausted") && s_Applier.invokes == 1);
    }
}
