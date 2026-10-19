// M7-B2: the motion session's retained record, its restoration (glacier-relay
// docs/design/M7_NETWORK_GHOST.md section 17.2; the M6 transform result's field names), the
// at-baseline release without a call, and the native mirror of the exclusivity with both M6 entry
// versions (section 15.4), driven through Motion::Controller and the real CommandScheduler with the
// FAKE in-memory link and the FAKE transform applier (scripted words; never an engine).

#include <algorithm>
#include <cstdio>
#include <string>

#include "CommandScheduler.h"
#include "MotionTestRig.h"
#include "TestHarness.h"
#include "TestListener.h"

using CommandProtocol::Status;
using MotionTest::Field;
using MotionTest::Observed;
using MotionTest::Rig;

namespace
{
    bool Contains(const std::string& p_Text, const std::string& p_Part)
    {
        return p_Text.find(p_Part) != std::string::npos;
    }

    // A session opened, moved once by +0.1 m in x, and closed cleanly by BEAM: a restorable record
    // whose final words are the displaced ones.
    void DisplacedAndClosed(Rig& p_Rig)
    {
        p_Rig.Remember();
        CHECK(p_Rig.Open().status == Status::Applied);
        p_Rig.Target(1, p_Rig.At(0.1, 0.0));
        p_Rig.Step();
        CHECK(p_Rig.applier.invokes == 1);
        p_Rig.link.Close(MotionTest::k_Session, "stream_end:course_complete", p_Rig.now);
        p_Rig.Step();
        CHECK(!p_Rig.C().SessionOpen() && p_Rig.C().Record() && p_Rig.C().Record()->restorable);
    }

    void Restoration()
    {
        // The valid restoration: one call with the opening preimage; every section 17.2 fact on the
        // call frame, with the M6 result's names; the record released; nothing further held.
        {
            Rig s_Rig;
            DisplacedAndClosed(s_Rig);
            const FloatWords::Words12 s_Opening = s_Rig.C().Record()->opening_world;
            const FloatWords::Words12 s_Final = s_Rig.World();
            CHECK(s_Final != s_Opening && s_Rig.C().Record()->final_world == s_Final);
            CHECK(s_Rig.C().HoldsTransform(s_Rig.frame, [&] { return s_Rig.applier.Current(false, s_Rig.applier.world.fresh_generation); }));

            const auto s_Result = s_Rig.Restore();
            CHECK(s_Result.status == Status::Applied && s_Rig.applier.invokes == 2 && s_Rig.applier.last_target == s_Opening);
            CHECK(Observed(s_Result, "mode") == "restore" && Observed(s_Result, "restore_of") == MotionTest::k_OpenId);
            CHECK(Observed(s_Result, "requested_world_words") == FloatWords::Encode(s_Opening) && Observed(s_Result, "postimage_world_words") == FloatWords::Encode(s_Opening));
            CHECK(Observed(s_Result, "world_match") == "target" && Observed(s_Result, "postimage_dirty") == "false" && Observed(s_Result, "post_same_entity") == "true");
            CHECK(Observed(s_Result, "postimage_parent") == FakeTransform::k_Brick && Observed(s_Result, "postimage_local_words") == FloatWords::Encode(s_Rig.applier.world.local));
            CHECK(Observed(s_Result, "generation_post") == FakeTransform::k_Gen1 && Observed(s_Result, "hero_control") == "unchanged");
            CHECK(Observed(s_Result, "setter_called") == "true" && Observed(s_Result, "invocation") == "called" && Observed(s_Result, "effect") == "changed");
            CHECK(Observed(s_Result, "preimage_world_words") == FloatWords::Encode(s_Final) && Observed(s_Result, "apply_log") == "written");
            CHECK(CommandProtocol::EncodeResult(s_Result).size() <= CommandProtocol::k_MaxLineBytes);
            CHECK(s_Rig.C().Record()->restored && !s_Rig.C().Record()->restorable && s_Rig.C().GetStats().restores_proven == 1);
            CHECK(!s_Rig.C().HoldsTransform(s_Rig.frame, nullptr));

            // one restore per session: a second one is refused with no call
            const auto s_Again = s_Rig.Restore([&](CommandProtocol::Command& c) { c.motion->expect_world = s_Rig.World(); c.motion->target_world = s_Final; });
            CHECK(s_Again.status == Status::PreconditionFailed && Contains(s_Again.reason, "already_restored") && s_Rig.applier.invokes == 2);
        }

        // The at-baseline release: a session that ended at its opening words releases on the next
        // exclusive read, with no call (BEAM's confirm_baseline meets it without a release command).
        {
            Rig s_Rig;
            s_Rig.Remember();
            CHECK(s_Rig.Open().status == Status::Applied);
            s_Rig.Target(1, s_Rig.At(0.1, 0.0));
            s_Rig.Step();
            s_Rig.Target(2, s_Rig.At(0.0, 0.0));
            s_Rig.Step(50);
            CHECK(s_Rig.applier.invokes == 2 && s_Rig.World().w[9] == s_Rig.X0Opening());
            s_Rig.link.Close(MotionTest::k_Session, "stream_end:course_complete", s_Rig.now);
            s_Rig.Step();
            CHECK(s_Rig.C().Record() && s_Rig.C().Record()->restorable);
            CHECK(!s_Rig.C().HoldsTransform(s_Rig.frame, [&] { return s_Rig.applier.Current(false, s_Rig.applier.world.fresh_generation); }));
            CHECK(s_Rig.C().GetStats().released_at_baseline == 1 && !s_Rig.C().Record() && s_Rig.applier.invokes == 2);
        }
        {
            // not released while displaced, nor when the parent or the generation differs
            Rig s_Rig;
            DisplacedAndClosed(s_Rig);
            auto s_Read = [&] { return s_Rig.applier.Current(false, s_Rig.applier.world.fresh_generation); };
            CHECK(s_Rig.C().HoldsTransform(s_Rig.frame, s_Read));
            s_Rig.applier.world.world = s_Rig.C().Record()->opening_world;
            s_Rig.applier.world.parent = "feed000000000bad";
            CHECK(s_Rig.C().HoldsTransform(s_Rig.frame, s_Read));
            s_Rig.applier.world.parent = FakeTransform::k_Brick;
            s_Rig.applier.world.fresh_generation = FakeTransform::k_Gen2;
            CHECK(s_Rig.C().HoldsTransform(s_Rig.frame, s_Read));
            s_Rig.applier.world.fresh_generation = FakeTransform::k_Gen1;
            s_Rig.applier.world.dirty = true;
            CHECK(s_Rig.C().HoldsTransform(s_Rig.frame, s_Read));
            s_Rig.applier.world.dirty = false;
            CHECK(!s_Rig.C().HoldsTransform(s_Rig.frame, s_Read) && s_Rig.applier.invokes == 1); // the engine put it back: observed, released, no write
        }

        // Every refusal of a restore: a non-application with no invocation and no effect reported;
        // the record keeps its state (a later valid restore is still possible).
        struct Case
        {
            const char* label;
            std::function<void(Rig&)> setup;
            std::function<void(CommandProtocol::Command&)> edit;
            const char* reason;
        };
        const Case s_Cases[] = {
            {"restore_of another open", nullptr, [](CommandProtocol::Command& c) { c.motion->restore_of = "c0ffee00-0000-4000-8000-0000000000ff"; }, "operation_record_mismatch"},
            {"a target that is not the opening preimage", nullptr, [](CommandProtocol::Command& c) { c.motion->target_world->w[11] ^= 1; }, "restore_target_mismatch"},
            {"another session", nullptr, [](CommandProtocol::Command& c) { c.motion->session_id = MotionTest::k_Session2; }, "operation_record_missing"},
            {"moved since the close", [](Rig& r) { r.applier.world.world.w[10] = MotionTest::Word(MotionTest::Value(r.applier.world.world.w[10]) + 0.01f); }, nullptr, "current_not_postimage"},
            {"stale expectation", nullptr, [](CommandProtocol::Command& c) { c.motion->expect_world.w[10] ^= 1; }, "expect_world_mismatch"},
            {"dirty preimage", [](Rig& r) { r.applier.world.dirty = true; }, nullptr, "preimage_dirty"},
            {"generation", [](Rig& r) { r.applier.world.fresh_generation = FakeTransform::k_Gen2; }, nullptr, "restore_generation_changed"},
            {"parent", [](Rig& r) { r.applier.world.parent = "feed000000000bad"; }, [](CommandProtocol::Command& c) { c.motion->expect_parent = "feed000000000bad"; }, "restore_parent_not_opening"},
            {"command link", [](Rig& r) { r.command_gen = MotionTest::k_CommandGen + 1; }, nullptr, "restore_link_changed"},
            {"candidate in the registry", [](Rig& r) { r.applier.world.in_registry = true; }, nullptr, "candidate_in_registry"},
            {"latched", [](Rig& r) { r.applier.latched = true; }, nullptr, "effectful_latched"},
        };
        for (const auto& s_Case : s_Cases)
        {
            Rig s_Rig;
            DisplacedAndClosed(s_Rig);
            if (s_Case.setup)
                s_Case.setup(s_Rig);
            const auto s_Result = s_Rig.Restore([&](CommandProtocol::Command& c) {
                c.motion->expect_world = s_Rig.World();
                if (s_Case.edit)
                    s_Case.edit(c);
            });
            const bool s_Ok = (s_Result.status == Status::PreconditionFailed || s_Result.status == Status::Unavailable) && Contains(s_Result.reason, s_Case.reason)
                && s_Rig.applier.invokes == 1 && Observed(s_Result, "setter_called") == "false" && Observed(s_Result, "effect") == "none" && Observed(s_Result, "invocation") == "not_called"
                && (s_Case.edit && std::string(s_Case.label) == "another session" ? true : (s_Rig.C().Record() && s_Rig.C().Record()->restorable));
            if (!s_Ok)
                std::printf("  restore refusal '%s': status %s reason '%s' invokes %d\n", s_Case.label, CommandProtocol::StatusText(s_Result.status), s_Result.reason.c_str(), s_Rig.applier.invokes);
            CHECK(s_Ok);
        }

        // The checked line not accepted: a proven non-application; the record stays restorable and a
        // later, separately issued restore can still succeed (the BEAM policy decides whether to).
        {
            Rig s_Rig;
            DisplacedAndClosed(s_Rig);
            CommandProtocol::Result s_Result;
            {
                MotionTest::LogHook::Scope s_Hook("applying candidate.motion.session/1 mode restore", nullptr, true);
                s_Result = s_Rig.Restore([&](CommandProtocol::Command& c) { c.motion->expect_world = s_Rig.World(); });
            }
            CHECK(s_Result.status == Status::Unavailable && Contains(s_Result.reason, "apply_log_unwritten") && s_Rig.applier.invokes == 1 && Observed(s_Result, "setter_called") == "false");
            CHECK(s_Rig.C().Record()->restorable && !s_Rig.C().Record()->invalidated);
            const auto s_Later = s_Rig.Restore([&](CommandProtocol::Command& c) { c.motion->expect_world = s_Rig.World(); }, MotionTest::k_Session, "c0ffee00-0000-4000-8000-000000000009");
            CHECK(s_Later.status == Status::Applied && Observed(s_Later, "world_match") == "target" && s_Rig.applier.invokes == 2);
        }

        // An attempted restoration without proof (inexact, dirty, another entity, control changed):
        // applied with the effect as classified, the record invalidated for the process — no second
        // restore, no baseline release, the M6 entries held.
        const FakeTransform::Mode s_Uncertain[] = {FakeTransform::Mode::Elsewhere, FakeTransform::Mode::DirtyPost, FakeTransform::Mode::PostEntityChange, FakeTransform::Mode::ControlChanges, FakeTransform::Mode::ReadBackUnavailable};
        for (const auto s_Mode : s_Uncertain)
        {
            Rig s_Rig;
            DisplacedAndClosed(s_Rig);
            s_Rig.applier.world.mode = s_Mode;
            const auto s_Result = s_Rig.Restore([&](CommandProtocol::Command& c) { c.motion->expect_world = s_Rig.World(); });
            const bool s_Ok = s_Result.status == Status::Applied && Observed(s_Result, "setter_called") == "true" && s_Rig.applier.invokes == 2
                && s_Rig.C().Record()->invalidated && !s_Rig.C().Record()->restorable && s_Rig.C().GetStats().restores_proven == 0;
            if (!s_Ok)
                std::printf("  uncertain restore mode %d: status %s effect %s world_match %s\n", static_cast<int>(s_Mode), CommandProtocol::StatusText(s_Result.status), Observed(s_Result, "effect").c_str(), Observed(s_Result, "world_match").c_str());
            CHECK(s_Ok);
            s_Rig.applier.world.mode = FakeTransform::Mode::ChangeToTarget;
            s_Rig.applier.world.resolved = true;
            s_Rig.applier.world.dirty = false;
            const auto s_Second = s_Rig.Restore([&](CommandProtocol::Command& c) { c.motion->expect_world = s_Rig.World(); }, MotionTest::k_Session, "c0ffee00-0000-4000-8000-00000000000a");
            CHECK(s_Second.status != Status::Applied && Contains(s_Second.reason, "operation_record_invalidated") && s_Rig.applier.invokes == 2);
            s_Rig.applier.world.world = s_Rig.C().Record()->opening_world;
            CHECK(s_Rig.C().HoldsTransform(s_Rig.frame, [&] { return s_Rig.applier.Current(false, s_Rig.applier.world.fresh_generation); }));
        }

        // An unclean session (an uncertain apply) is never restorable.
        {
            Rig s_Rig;
            s_Rig.Remember();
            CHECK(s_Rig.Open().status == Status::Applied);
            s_Rig.applier.world.mode = FakeTransform::Mode::Elsewhere;
            s_Rig.Target(1, s_Rig.At(0.1, 0.0));
            s_Rig.Step();
            s_Rig.applier.world.mode = FakeTransform::Mode::ChangeToTarget;
            const auto s_Result = s_Rig.Restore([&](CommandProtocol::Command& c) { c.motion->expect_world = s_Rig.World(); });
            CHECK(s_Result.status != Status::Applied && Contains(s_Result.reason, "operation_record_invalidated") && s_Rig.applier.invokes == 1);
        }

        // The restore entry without the effectful setting (no applier): unavailable, nothing called.
        {
            Rig s_Rig(false);
            const auto s_Result = s_Rig.Restore();
            CHECK(s_Result.status == Status::Unavailable && Contains(s_Result.reason, "applier_unavailable"));
        }

        // The pure gate with another session open (p_Holds) refuses with motion_session_held.
        {
            Rig s_Rig;
            DisplacedAndClosed(s_Rig);
            const auto s_Command = [&] {
                auto c = s_Rig.SessionCommand(CommandProtocol::MotionSessionMode::Restore, MotionTest::k_RestoreId, MotionTest::k_Session);
                c.motion->target_world = s_Rig.C().Record()->opening_world;
                c.motion->restore_of = MotionTest::k_OpenId;
                return c;
            }();
            const auto s_Pending = s_Rig.Pending(s_Command);
            const auto s_Context = s_Rig.Context(s_Pending);
            CommandProtocol::Result s_Result;
            TransformApply::MotionRestoreVerdict s_Verdict;
            TransformApply::ApplyMotionRestore(s_Context, s_Rig.applier, &*s_Rig.C().Record(), true, 0, s_Result, s_Verdict);
            CHECK(s_Result.status == Status::PreconditionFailed && Contains(s_Result.reason, "motion_session_held") && !s_Verdict.invoked && s_Rig.applier.invokes == 1);
        }
    }

    // The scheduler's M6 path with the motion controller on the frame: refused while a session holds
    // the candidate (open or retained), then admitted after the restoration.
    CommandProtocol::Command StepY(const std::string& p_Id, const FloatWords::Words12& p_World, const FloatWords::Words7& p_Local)
    {
        CommandProtocol::Command s_Command;
        s_Command.command_id = p_Id;
        s_Command.link_generation = MotionTest::k_CommandGen;
        s_Command.ttl_ms = 5000;
        s_Command.name = "candidate.transform.set";
        s_Command.version = 2;
        s_Command.entry = CommandProtocol::Entry::CandidateTransformSet2;
        s_Command.mission_generation_precondition = FakeTransform::k_Gen1;
        CommandProtocol::TransformRequest s_Request;
        s_Request.mode = CommandProtocol::TransformMode::StepY;
        s_Request.version = 2;
        s_Request.expect_world = p_World;
        s_Request.expect_local = p_Local;
        s_Request.expect_parent = FakeTransform::k_Brick;
        s_Command.transform = s_Request;
        return s_Command;
    }

    void Exclusion()
    {
        Rig s_Rig;
        CommandQueue s_Queue;
        std::vector<CommandProtocol::Result> s_Emitted;
        CommandScheduler s_Scheduler(s_Queue, [&](const CommandProtocol::Result& p_Result) { s_Emitted.push_back(p_Result); }, PreconditionEvaluator::Rules{0, 0});

        auto s_Tick = [&] {
            CommandScheduler::FrameObservation s_Frame;
            s_Frame.frame = ++s_Rig.frame;
            s_Frame.now_mono_ms = s_Rig.now;
            s_Frame.thread_id = s_Rig.thread;
            s_Frame.scene_available = true;
            s_Frame.scene.available = true;
            s_Frame.scene.scene_loaded = true;
            s_Frame.scene.loading_stage = 8;
            s_Frame.scene.scene_type = "mission";
            s_Frame.scene.scene_resource = s_Rig.scene;
            s_Frame.mission_playing = true;
            s_Frame.previous_frame_playing = true;
            s_Frame.mission_generation = s_Rig.generation;
            s_Frame.link_generation = MotionTest::k_CommandGen;
            s_Frame.relay_transition_epoch = s_Rig.epoch;
            s_Frame.rise_frame = 1;
            s_Frame.rise_mono_ms = 1;
            s_Frame.scene_resource_same_as_rise = true;
            s_Frame.contract_started_seen = s_Rig.generation;
            s_Frame.transform_inspector = [&] { return s_Rig.applier.Current(false, s_Rig.applier.world.fresh_generation); };
            s_Frame.transform_applier = &s_Rig.applier;
            s_Frame.link_generation_now = [] { return MotionTest::k_CommandGen; };
            s_Frame.motion = &s_Rig.C();
            s_Queue.PublishSnapshot(CommandQueue::FrameSnapshot{s_Rig.generation, s_Rig.epoch});
            s_Scheduler.Tick(s_Frame);
            s_Rig.C().Step(s_Rig.Frame());
        };
        auto s_Push = [&](const CommandProtocol::Command& p_Command) {
            s_Queue.PublishSnapshot(CommandQueue::FrameSnapshot{s_Rig.generation, s_Rig.epoch});
            s_Queue.Push(p_Command, MotionTest::k_CommandGen, s_Rig.now);
        };

        // the motion open itself through the scheduler
        s_Rig.Remember();
        s_Push(s_Rig.SessionCommand(CommandProtocol::MotionSessionMode::Open, MotionTest::k_OpenId, MotionTest::k_Session));
        s_Tick();
        CHECK(!s_Emitted.empty() && s_Emitted.back().status == Status::Applied && s_Rig.C().SessionOpen() && s_Scheduler.GetStats().motion_dispatched == 1);

        // M6 v2 step while the session is open: refused (motion_session_held), no call
        s_Push(StepY("c0ffee00-0000-4000-8000-0000000000b1", s_Rig.World(), s_Rig.applier.world.local));
        s_Tick();
        CHECK(s_Emitted.back().status == Status::PreconditionFailed && Contains(s_Emitted.back().reason, "motion_session_held") && s_Rig.applier.invokes == 0);
        CHECK(s_Scheduler.TransformLedger().forward_budget == 1 && s_Scheduler.GetStats().transform_motion_held == 1);

        // a target applied, then BEAM closes: the record is retained (displaced) and still holds M6
        s_Rig.Target(1, s_Rig.At(0.1, 0.0));
        s_Rig.now += 16;
        s_Tick();
        CHECK(s_Rig.applier.invokes == 1);
        s_Rig.link.Close(MotionTest::k_Session, "stream_end:course_complete", s_Rig.now);
        s_Rig.now += 16;
        s_Tick();
        CHECK(!s_Rig.C().SessionOpen() && s_Rig.C().Record() && s_Rig.C().Record()->restorable);
        s_Push(StepY("c0ffee00-0000-4000-8000-0000000000b2", s_Rig.World(), s_Rig.applier.world.local));
        s_Tick();
        CHECK(Contains(s_Emitted.back().reason, "motion_session_held") && s_Rig.applier.invokes == 1);

        // the motion restore through the scheduler, then the M6 step is admitted and applied
        auto s_Restore = s_Rig.SessionCommand(CommandProtocol::MotionSessionMode::Restore, MotionTest::k_RestoreId, MotionTest::k_Session);
        s_Restore.motion->target_world = s_Rig.C().Record()->opening_world;
        s_Restore.motion->restore_of = MotionTest::k_OpenId;
        s_Push(s_Restore);
        s_Tick();
        CHECK(s_Emitted.back().status == Status::Applied && Observed(s_Emitted.back(), "world_match") == "target" && s_Rig.applier.invokes == 2);
        s_Push(StepY("c0ffee00-0000-4000-8000-0000000000b3", s_Rig.World(), s_Rig.applier.world.local));
        s_Tick();
        CHECK(s_Emitted.back().status == Status::Applied && s_Rig.applier.invokes == 3 && s_Scheduler.TransformLedger().forward_budget == 0);

        // and now the M6 forward is unrestored: a motion open is refused natively
        s_Push(s_Rig.SessionCommand(CommandProtocol::MotionSessionMode::Open, "c0ffee00-0000-4000-8000-0000000000b4", MotionTest::k_Session2));
        s_Tick();
        CHECK(s_Emitted.back().status != Status::Applied && Contains(s_Emitted.back().reason, "m6_forward_unrestored") && !s_Rig.C().SessionOpen());
    }

    void OneCallPerFrame()
    {
        // At most one setter call per frame across both paths: a call already made on this frame (by
        // the M6 path) defers the session's pending target to the next frame, never a second call.
        Rig s_Rig;
        s_Rig.Remember();
        CHECK(s_Rig.Open().status == Status::Applied);
        s_Rig.Target(1, s_Rig.At(0.05, 0.0));
        s_Rig.C().NoteInvocation(s_Rig.frame + 1);
        s_Rig.Step();
        CHECK(s_Rig.applier.invokes == 0 && s_Rig.C().GetStats().two_writes_prevented == 1 && s_Rig.C().SessionOpen());
        s_Rig.Step();
        CHECK(s_Rig.applier.invokes == 1 && s_Rig.C().InvokedOnFrame(s_Rig.frame));
    }

    void MotionOffIsM6()
    {
        // Without a controller on the frame (motion off): the motion entries are rejected and the M6
        // path is exactly M6's (no hold).
        FakeTransform::Applier s_Applier;
        CommandQueue s_Queue;
        std::vector<CommandProtocol::Result> s_Emitted;
        CommandScheduler s_Scheduler(s_Queue, [&](const CommandProtocol::Result& p_Result) { s_Emitted.push_back(p_Result); }, PreconditionEvaluator::Rules{0, 0});
        Rig s_Rig;
        auto s_Command = s_Rig.SessionCommand(CommandProtocol::MotionSessionMode::Open, MotionTest::k_OpenId, MotionTest::k_Session);
        s_Queue.Push(s_Command, MotionTest::k_CommandGen, s_Rig.now);
        CommandScheduler::FrameObservation s_Frame;
        s_Frame.frame = 1;
        s_Frame.now_mono_ms = s_Rig.now;
        s_Frame.link_generation = MotionTest::k_CommandGen;
        s_Frame.transform_applier = &s_Applier;
        s_Scheduler.Tick(s_Frame);
        CHECK(!s_Emitted.empty() && s_Emitted.back().status == Status::Rejected && Contains(s_Emitted.back().reason, "motion disabled") && s_Applier.observes == 0);
    }
}

namespace
{
    // The M6 restore's own confirmation flag (the motion exclusion's input), set by the unchanged M6
    // apply path on the restore's frame: true only for an exact, control-unchanged restoration.
    void M6RestoreConfirmation()
    {
        for (const bool s_ControlChanges : {false, true})
        {
            Rig s_Rig;
            TransformApply::Ledger s_Ledger;
            auto s_Run = [&](const CommandProtocol::Command& p_Command) {
                const auto s_Pending = s_Rig.Pending(p_Command);
                CommandProtocol::Result s_Result;
                TransformApply::Apply(s_Rig.Context(s_Pending), s_Rig.applier, s_Ledger, s_Result);
                return s_Result;
            };
            const FloatWords::Words12 s_Pre = s_Rig.World();
            CHECK(s_Run(StepY("c0ffee00-0000-4000-8000-0000000000c1", s_Pre, s_Rig.applier.world.local)).status == Status::Applied);
            CHECK(s_Ledger.state == TransformApply::Ledger::State::ForwardInvoked && !s_Ledger.restore_confirmed);
            auto s_Restore = StepY("c0ffee00-0000-4000-8000-0000000000c2", s_Rig.World(), s_Rig.applier.world.local);
            s_Restore.transform->mode = CommandProtocol::TransformMode::Restore;
            s_Restore.transform->target_world = s_Pre;
            s_Restore.transform->restore_of = std::string("c0ffee00-0000-4000-8000-0000000000c1");
            if (s_ControlChanges)
                s_Rig.applier.world.mode = FakeTransform::Mode::ControlChanges;
            const auto s_Result = s_Run(s_Restore);
            CHECK(s_Result.status == Status::Applied && s_Ledger.state != TransformApply::Ledger::State::ForwardInvoked);
            CHECK(s_Ledger.restore_confirmed == !s_ControlChanges);
        }
    }

    // The motion path's identity rule is the M6 gate's steps 1-3, rule for rule and in order, on every
    // FAKE world variant (EvaluateTransform lists those reasons first for a supported request).
    void IdentityRuleIsM6s()
    {
        const std::function<void(FakeTransform::World&)> s_Variants[] = {
            [](FakeTransform::World&) {},
            [](FakeTransform::World& w) { w.brick_present = false; },
            [](FakeTransform::World& w) { w.brick_present = false; w.scan_complete = false; },
            [](FakeTransform::World& w) { w.resolved = false; },
            [](FakeTransform::World& w) { w.index_in_range = false; },
            [](FakeTransform::World& w) { w.hitman5 = false; },
            [](FakeTransform::World& w) { w.id_match = false; },
            [](FakeTransform::World& w) { w.owner_match = false; },
            [](FakeTransform::World& w) { w.distinct = false; },
            [](FakeTransform::World& w) { w.spatial = false; },
            [](FakeTransform::World& w) { w.spatial_distinct = false; },
            [](FakeTransform::World& w) { w.registry_readable = false; },
            [](FakeTransform::World& w) { w.in_registry = true; },
            [](FakeTransform::World& w) { w.inconclusive_slot = true; },
            [](FakeTransform::World& w) { w.hero_readable = false; },
            [](FakeTransform::World& w) { w.resolved = false; w.hero_readable = false; w.registry_readable = false; },
        };
        int s_Index = 0;
        for (const auto& s_Variant : s_Variants)
        {
            FakeTransform::Applier s_Applier;
            s_Variant(s_Applier.world);
            const auto s_Reads = s_Applier.Current(false, s_Applier.world.fresh_generation);
            const auto s_Motion = TransformApply::IdentityFailures(s_Reads);

            CommandProtocol::TransformRequest s_Request;
            s_Request.mode = CommandProtocol::TransformMode::StepY;
            s_Request.version = 2;
            s_Request.expect_world = s_Applier.world.world;
            s_Request.expect_local = s_Applier.world.local;
            s_Request.expect_parent = s_Applier.world.parent;
            TransformApply::Ledger s_Ledger;
            TransformApply::GateInput s_Input;
            s_Input.request = &s_Request;
            s_Input.reads = &s_Reads;
            s_Input.ledger = &s_Ledger;
            s_Input.link_generation_now = 1;
            const auto s_M6 = TransformApply::EvaluateTransform(s_Input).reasons;
            const bool s_Prefix = s_M6.size() >= s_Motion.size() && std::equal(s_Motion.begin(), s_Motion.end(), s_M6.begin());
            bool s_NoMoreIdentity = true;
            for (size_t i = s_Motion.size(); i < s_M6.size(); ++i)
                if (s_M6[i] <= TransformApply::Reason::HeroControlUnavailable)
                    s_NoMoreIdentity = false;
            if (!(s_Prefix && s_NoMoreIdentity))
                std::printf("  identity variant %d: motion '%s' m6 '%s'\n", s_Index, TransformApply::ReasonsText(s_Motion).c_str(), TransformApply::ReasonsText(s_M6).c_str());
            CHECK(s_Prefix && s_NoMoreIdentity);
            ++s_Index;
        }
    }
}

void RunMotionRestoreTests()
{
    M6RestoreConfirmation();
    IdentityRuleIsM6s();
    Restoration();
    Exclusion();
    OneCallPerFrame();
    MotionOffIsM6();
}
