// M7-B2: the destination session's native enforcement (glacier-relay docs/design/M7_NETWORK_GHOST.md
// sections 15.3-15.5 and 17), driven through Motion::Controller with the FAKE in-memory motion link,
// the FAKE transform applier (scripted words; never an engine) and a controlled native clock. Every
// boundary is checked at, before and after its instant, each with an in-time control; "invokes" is
// the fake setter's own call count, so "no invocation" is the call never made.

#include <cstdio>
#include <string>

#include "MotionTestRig.h"
#include "TestHarness.h"

using CommandProtocol::Status;
using MotionTest::Field;
using MotionTest::Rig;

namespace
{
    bool Contains(const std::string& p_Text, const std::string& p_Part)
    {
        return p_Text.find(p_Part) != std::string::npos;
    }

    // An opened session at the fake's candidate words; the opening x / y remembered.
    void Opened(Rig& p_Rig, std::function<void(CommandProtocol::Command&)> p_Edit = nullptr)
    {
        p_Rig.Remember();
        const auto s_Result = p_Rig.Open(p_Edit);
        if (s_Result.status != Status::Applied)
            std::printf("  open refused: %s\n", s_Result.reason.c_str());
        CHECK(s_Result.status == Status::Applied && p_Rig.C().SessionOpen());
    }

    std::string LastAckOutcome(const Rig& p_Rig)
    {
        const auto s_Acks = p_Rig.Acks();
        return s_Acks.empty() ? std::string() : Field(s_Acks.back(), "outcome") + "/" + Field(s_Acks.back(), "reason");
    }

    std::string CloseReason(const Rig& p_Rig)
    {
        const auto s_Closed = p_Rig.Closeds();
        return s_Closed.empty() ? std::string("<none>") : Field(s_Closed.back(), "reason");
    }

    void Admission()
    {
        // Valid open: admission only — the frame's reads, released; no setter call.
        {
            Rig s_Rig;
            s_Rig.Remember();
            const auto s_Result = s_Rig.Open();
            CHECK(s_Result.status == Status::Applied);
            CHECK(MotionTest::Observed(s_Result, "opening_world") == FloatWords::Encode(s_Rig.World()));
            CHECK(MotionTest::Observed(s_Result, "session_open") == "true" && MotionTest::Observed(s_Result, "setter_called") == "false");
            CHECK(MotionTest::Observed(s_Result, "effect") == "none" && MotionTest::Observed(s_Result, "invocation") == "not_called");
            CHECK(s_Rig.applier.invokes == 0 && s_Rig.applier.observes == 1 && s_Rig.applier.releases == 1);
            CHECK(s_Rig.C().GetStats().sessions_opened == 1 && s_Rig.C().SessionsRemaining() == 0);
            CHECK(CommandProtocol::EncodeResult(s_Result).size() <= CommandProtocol::k_MaxLineBytes);
        }

        // Refusals: every one a non-application (no session, no call), each naming its reason.
        struct Case
        {
            const char* label;
            std::function<void(Rig&)> setup;
            std::function<void(CommandProtocol::Command&)> edit;
            Status status;
            const char* reason;
        };
        const Case s_Cases[] = {
            {"expectation world", nullptr, [](CommandProtocol::Command& c) { c.motion->expect_world.w[11] ^= 1; }, Status::PreconditionFailed, "expect_world_mismatch"},
            {"expectation local", nullptr, [](CommandProtocol::Command& c) { c.motion->expect_local.w[0] ^= 1; }, Status::PreconditionFailed, "expect_local_mismatch"},
            {"expectation parent", nullptr, [](CommandProtocol::Command& c) { c.motion->expect_parent = "none"; }, Status::PreconditionFailed, "expect_parent_mismatch"},
            {"motion link generation", nullptr, [](CommandProtocol::Command& c) { c.motion->motion_link_generation = MotionTest::k_MotionGen + 1; }, Status::PreconditionFailed, "motion_link_generation_mismatch"},
            {"motion link unbound", [](Rig& r) { r.link.binding.bound = false; }, nullptr, Status::Unavailable, "motion_link_not_bound"},
            {"motion link of another command link", [](Rig& r) { r.link.binding.command_generation = MotionTest::k_CommandGen + 1; }, nullptr, Status::PreconditionFailed, "motion_link_not_on_this_command_link"},
            {"dirty preimage", [](Rig& r) { r.applier.world.dirty = true; }, nullptr, Status::PreconditionFailed, "preimage_dirty"},
            {"candidate in the registry", [](Rig& r) { r.applier.world.in_registry = true; }, nullptr, Status::PreconditionFailed, "candidate_in_registry"},
            {"candidate unresolved", [](Rig& r) { r.applier.world.resolved = false; }, nullptr, Status::Unavailable, "candidate_unresolved"},
            {"candidate is the local hero", [](Rig& r) { r.applier.world.distinct = false; }, nullptr, Status::PreconditionFailed, "candidate_not_distinct"},
            {"fresh generation", [](Rig& r) { r.applier.world.fresh_generation = FakeTransform::k_Gen2; }, nullptr, Status::PreconditionFailed, "generation_mismatch_fresh"},
            {"latched", [](Rig& r) { r.applier.latched = true; }, nullptr, Status::Unavailable, "effectful_latched"},
            {"mission not playing", [](Rig& r) { r.playing = false; }, nullptr, Status::PreconditionFailed, "mission_not_playing"},
            {"m6 forward unrestored", [](Rig& r) { r.m6.forward_budget = 0; r.m6.state = TransformApply::Ledger::State::ForwardInvoked; }, nullptr, Status::PreconditionFailed, "m6_forward_unrestored"},
            {"m6 restore not confirmed", [](Rig& r) { r.m6.forward_budget = 0; r.m6.state = TransformApply::Ledger::State::Restored; r.m6.restore_confirmed = false; }, nullptr, Status::PreconditionFailed, "m6_forward_unrestored"},
            {"m6 forward invalidated", [](Rig& r) { r.m6.forward_budget = 0; r.m6.state = TransformApply::Ledger::State::Invalidated; }, nullptr, Status::PreconditionFailed, "m6_forward_unrestored"},
        };
        for (const auto& s_Case : s_Cases)
        {
            Rig s_Rig;
            if (s_Case.setup)
                s_Case.setup(s_Rig);
            const auto s_Result = s_Rig.Open(s_Case.edit);
            const bool s_Ok = s_Result.status == s_Case.status && Contains(s_Result.reason, s_Case.reason) && !s_Rig.C().SessionOpen() && s_Rig.applier.invokes == 0
                && MotionTest::Observed(s_Result, "setter_called") == "false" && s_Rig.C().GetStats().sessions_opened == 0;
            if (!s_Ok)
                std::printf("  open refusal '%s': status %s reason '%s'\n", s_Case.label, CommandProtocol::StatusText(s_Result.status), s_Result.reason.c_str());
            CHECK(s_Ok);
        }

        // Without the effectful setting the controller has no applier: never opened.
        {
            Rig s_Rig(false);
            const auto s_Result = s_Rig.Open();
            CHECK(s_Result.status == Status::Unavailable && Contains(s_Result.reason, "applier_unavailable") && !s_Rig.C().SessionOpen());
        }

        // An M6 restoration confirmed on its frame no longer holds a motion open.
        {
            Rig s_Rig;
            s_Rig.m6.forward_budget = 0;
            s_Rig.m6.state = TransformApply::Ledger::State::Restored;
            s_Rig.m6.restore_confirmed = true;
            CHECK(s_Rig.Open().status == Status::Applied);
        }

        // One session at a time; ids never reused; the budget never reset.
        {
            Rig s_Rig(true, 2);
            Opened(s_Rig);
            const auto s_Second = s_Rig.Open(nullptr, MotionTest::k_Session2, "c0ffee00-0000-4000-8000-000000000003");
            CHECK(s_Second.status == Status::PreconditionFailed && Contains(s_Second.reason, "session_open"));
            // closed at its opening words (no target): the record is at baseline and releases on the next read
            s_Rig.link.Close(MotionTest::k_Session, "operator", s_Rig.now);
            s_Rig.Step();
            CHECK(!s_Rig.C().SessionOpen() && CloseReason(s_Rig) == "beam:operator");
            const auto s_Reused = s_Rig.Open();
            CHECK(s_Reused.status == Status::PreconditionFailed && Contains(s_Reused.reason, "session_id_reused"));
            const auto s_Third = s_Rig.Open(nullptr, MotionTest::k_Session2, "c0ffee00-0000-4000-8000-000000000004");
            CHECK(s_Third.status == Status::Applied && s_Rig.C().GetStats().released_at_baseline == 1);
            s_Rig.link.Close(MotionTest::k_Session2, "operator", s_Rig.now);
            s_Rig.Step();
            const auto s_Fourth = s_Rig.Open(nullptr, "5e55a0b1-0000-4000-8000-000000000003", "c0ffee00-0000-4000-8000-000000000005");
            CHECK(s_Fourth.status == Status::PreconditionFailed && Contains(s_Fourth.reason, "session_budget_exhausted"));
        }
    }

    void Application()
    {
        // The acquisition (no exemption: within max_speed x credit of the opening words), then a
        // skipped_identical with no call, the reports and the exact postimage.
        {
            Rig s_Rig;
            Opened(s_Rig);
            s_Rig.Target(1, s_Rig.At(0.1, 0.0));
            s_Rig.Step();
            CHECK(s_Rig.applier.invokes == 1 && LastAckOutcome(s_Rig) == "applied/");
            const auto s_Ack = s_Rig.Acks().back();
            CHECK(Field(s_Ack, "exact") == "true" && Field(s_Ack, "post") == FloatWords::Encode(&s_Rig.applier.world.world.w[9], 2) && Field(s_Ack, "seq") == "1");
            CHECK(s_Rig.applier.last_target.w[9] == s_Rig.At(0.1, 0.0).first && s_Rig.applier.last_target.w[11] == s_Rig.World().w[11]);
            // every word but x / y is the opening snapshot's in the request
            bool s_Untouched = true;
            for (size_t i = 0; i < 12; ++i)
                if (i != 9 && i != 10 && s_Rig.applier.last_target.w[i] != s_Rig.applier.preimage.w[i])
                    s_Untouched = false;
            CHECK(s_Untouched);

            s_Rig.Target(2, s_Rig.At(0.1, 0.0));
            s_Rig.Step(50);
            CHECK(s_Rig.applier.invokes == 1 && LastAckOutcome(s_Rig) == "skipped_identical/" && Field(s_Rig.Acks().back(), "post") == FloatWords::Encode(&s_Rig.applier.world.world.w[9], 2));

            s_Rig.Step(300);
            const auto s_Reports = s_Rig.Reports();
            CHECK(s_Reports.size() == 1);
            if (!s_Reports.empty())
                CHECK(Field(s_Reports[0], "received") == "2" && Field(s_Reports[0], "applied") == "1" && Field(s_Reports[0], "skipped") == "1" && Field(s_Reports[0], "refused") == "0"
                      && Field(s_Reports[0], "uncertain") == "0" && Field(s_Reports[0], "superseded") == "0" && Field(s_Reports[0], "last_applied_seq") == "1" && Field(s_Reports[0], "report_seq") == "1");
            // a slow frame: one report, never a catch-up burst
            s_Rig.Target(3, s_Rig.At(0.1, 0.0));
            s_Rig.Step(16);
            const size_t s_Before = s_Rig.Reports().size();
            s_Rig.Target(4, s_Rig.At(0.1, 0.0));
            s_Rig.Step(900); // within the silence of target 4 (received 16 ms ago... at the frame's start)
            CHECK(s_Rig.Reports().size() == s_Before + 1);
        }

        // The displacement bound, inclusive, on the exact binary32 boundary (and one step beyond).
        {
            Rig s_Rig;
            Opened(s_Rig);
            const auto s_Edge = MotionTest::Boundary(MotionTest::Value(s_Rig.X0Opening()), 0.2);
            s_Rig.Target(1, {MotionTest::Word(s_Edge.first), s_Rig.Y0Opening()});
            s_Rig.Step();
            CHECK(s_Rig.applier.invokes == 1 && LastAckOutcome(s_Rig) == "applied/");
        }
        {
            Rig s_Rig;
            Opened(s_Rig);
            const auto s_Edge = MotionTest::Boundary(MotionTest::Value(s_Rig.X0Opening()), 0.2);
            s_Rig.Target(1, {MotionTest::Word(s_Edge.second), s_Rig.Y0Opening()});
            s_Rig.Step();
            CHECK(s_Rig.applier.invokes == 0 && LastAckOutcome(s_Rig) == "refused/displacement" && CloseReason(s_Rig) == "refused:displacement" && !s_Rig.C().SessionOpen());
        }

        // The area, inclusive (half extent 0.1 m; the displacement cap 0.2 m does not decide), on each
        // axis and in each direction: the last binary32 inside is applied, the next one is refused.
        for (int s_Axis = 0; s_Axis < 4; ++s_Axis)
            for (int s_Beyond = 0; s_Beyond <= 1; ++s_Beyond)
            {
                Rig s_Rig;
                Opened(s_Rig, [](CommandProtocol::Command& c) { c.motion->limits.half_extent_mm = 100; });
                const bool s_OnX = s_Axis < 2;
                const bool s_Up = (s_Axis % 2) == 0;
                const float s_From = MotionTest::Value(s_OnX ? s_Rig.X0Opening() : s_Rig.Y0Opening());
                const auto s_Edge = s_Up ? MotionTest::Boundary(s_From, 0.1) : MotionTest::BoundaryDown(s_From, 0.1);
                const uint32_t s_Word = MotionTest::Word(s_Beyond ? s_Edge.second : s_Edge.first);
                s_Rig.Target(1, s_OnX ? std::make_pair(s_Word, s_Rig.Y0Opening()) : std::make_pair(s_Rig.X0Opening(), s_Word));
                s_Rig.Step();
                const bool s_Ok = s_Beyond ? (s_Rig.applier.invokes == 0 && LastAckOutcome(s_Rig) == "refused/outside_area" && !s_Rig.C().SessionOpen())
                                           : (s_Rig.applier.invokes == 1 && LastAckOutcome(s_Rig) == "applied/" && s_Rig.C().SessionOpen());
                if (!s_Ok)
                    std::printf("  area boundary axis %d beyond %d: invokes %d ack '%s'\n", s_Axis, s_Beyond, s_Rig.applier.invokes, LastAckOutcome(s_Rig).c_str());
                CHECK(s_Ok);
            }

        // The exact equality case (the bound representable: 0.125 m, the B1 boundary): a target at
        // exactly the bound is inside (inclusive), on each axis and direction, for the area (half
        // extent 125 mm) and for the displacement (1,250 mm/s x 100 ms); one binary32 step beyond is not.
        for (int s_Rule = 0; s_Rule < 2; ++s_Rule)
            for (int s_Axis = 0; s_Axis < 4; ++s_Axis)
                for (int s_Beyond = 0; s_Beyond <= 1; ++s_Beyond)
                {
                    Rig s_Rig;
                    Opened(s_Rig, [&](CommandProtocol::Command& c) {
                        if (s_Rule == 0)
                            c.motion->limits.half_extent_mm = 125; // the displacement cap 0.2 m does not decide
                        else
                            c.motion->limits.max_speed_mmps = 1250; // the area (8 m) does not decide
                    });
                    const bool s_OnX = s_Axis < 2;
                    const float s_Sign = (s_Axis % 2) == 0 ? 1.0f : -1.0f;
                    const float s_From = MotionTest::Value(s_OnX ? s_Rig.X0Opening() : s_Rig.Y0Opening());
                    const float s_Exact = s_From + s_Sign * 0.125f; // exactly representable at this magnitude
                    CHECK(std::fabs(static_cast<double>(s_Exact) - static_cast<double>(s_From)) == 0.125);
                    const uint32_t s_Word = MotionTest::Word(s_Beyond ? std::nextafter(s_Exact, s_Sign > 0 ? INFINITY : -INFINITY) : s_Exact);
                    s_Rig.Target(1, s_OnX ? std::make_pair(s_Word, s_Rig.Y0Opening()) : std::make_pair(s_Rig.X0Opening(), s_Word));
                    s_Rig.Step();
                    const char* s_Refusal = s_Rule == 0 ? "refused/outside_area" : "refused/displacement";
                    const bool s_Ok = s_Beyond ? (s_Rig.applier.invokes == 0 && LastAckOutcome(s_Rig) == s_Refusal)
                                               : (s_Rig.applier.invokes == 1 && LastAckOutcome(s_Rig) == "applied/");
                    if (!s_Ok)
                        std::printf("  exact boundary rule %d axis %d beyond %d: invokes %d ack '%s'\n", s_Rule, s_Axis, s_Beyond, s_Rig.applier.invokes, LastAckOutcome(s_Rig).c_str());
                    CHECK(s_Ok);
                }

        // The displacement bound in the negative direction and diagonally (both words move).
        {
            Rig s_Rig;
            Opened(s_Rig);
            const auto s_Edge = MotionTest::BoundaryDown(MotionTest::Value(s_Rig.Y0Opening()), 0.2);
            s_Rig.Target(1, {s_Rig.X0Opening(), MotionTest::Word(s_Edge.second)});
            s_Rig.Step();
            CHECK(s_Rig.applier.invokes == 0 && LastAckOutcome(s_Rig) == "refused/displacement");
        }
        {
            Rig s_Rig;
            Opened(s_Rig);
            s_Rig.Target(1, s_Rig.At(0.14, 0.14)); // 0.198 m: inside
            s_Rig.Step();
            CHECK(s_Rig.applier.invokes == 1 && LastAckOutcome(s_Rig) == "applied/");
            s_Rig.Target(2, s_Rig.At(0.29, 0.29)); // 0.212 m from the reference: outside
            s_Rig.Step(50);
            CHECK(s_Rig.applier.invokes == 1 && LastAckOutcome(s_Rig) == "refused/displacement");
        }

        // A far first target (a reconnect or a pause earns no credit): refused, never clamped.
        {
            Rig s_Rig;
            Opened(s_Rig);
            s_Rig.Step(900); // a pause shorter than the silence
            s_Rig.Target(1, s_Rig.At(0.5, 0.0));
            s_Rig.Step();
            CHECK(s_Rig.applier.invokes == 0 && LastAckOutcome(s_Rig) == "refused/displacement");
        }

        // One pending slot: a second target before the first's acknowledgement is refused (never
        // superseding it); application ends; the first is accounted refused before the close.
        {
            Rig s_Rig;
            Opened(s_Rig);
            s_Rig.Target(1, s_Rig.At(0.05, 0.0));
            s_Rig.Target(2, s_Rig.At(0.1, 0.0));
            s_Rig.Step();
            const auto s_Acks = s_Rig.Acks();
            CHECK(s_Rig.applier.invokes == 0 && s_Acks.size() == 2);
            if (s_Acks.size() == 2)
                CHECK(Field(s_Acks[0], "seq") == "2" && Field(s_Acks[0], "reason") == "pending_slot_occupied" && Field(s_Acks[1], "seq") == "1" && Field(s_Acks[1], "reason") == "session_closed");
            CHECK(CloseReason(s_Rig) == "refused:pending_slot_occupied");
            const auto s_Closed = s_Rig.Closeds();
            if (!s_Closed.empty())
                CHECK(Field(s_Closed[0], "applied") == "0" && Field(s_Closed[0], "last_applied_seq") == "" && Field(s_Closed[0], "clean") == "true");
        }

        // Duplicate and older sequence numbers: refused, never re-applied; the session ends.
        for (const uint64_t s_Again : {uint64_t(1), uint64_t(2)})
        {
            Rig s_Rig;
            Opened(s_Rig);
            s_Rig.Target(2, s_Rig.At(0.05, 0.0));
            s_Rig.Step();
            CHECK(s_Rig.applier.invokes == 1);
            s_Rig.Target(s_Again, s_Rig.At(0.1, 0.0));
            s_Rig.Step(50);
            CHECK(s_Rig.applier.invokes == 1 && LastAckOutcome(s_Rig) == "refused/duplicate_or_older" && !s_Rig.C().SessionOpen());
        }

        // A foreign target is counted and ignored: no acknowledgement, no refresh of the silence.
        {
            Rig s_Rig;
            Opened(s_Rig);
            const uint64_t s_Opened = s_Rig.now;
            s_Rig.Step(500);
            s_Rig.Target(1, s_Rig.At(0.05, 0.0), MotionTest::k_Foreign);
            s_Rig.Step(400);
            CHECK(s_Rig.C().SessionOpen() && s_Rig.Acks().empty() && s_Rig.C().GetStats().foreign_targets == 1);
            s_Rig.now = s_Opened + 1000 - 1;
            s_Rig.Step(0);
            CHECK(s_Rig.C().SessionOpen());
            s_Rig.Step(1); // exactly opened + 1000: the silence that the foreign target did not refresh
            CHECK(!s_Rig.C().SessionOpen() && CloseReason(s_Rig) == "target_silence");
        }

        // The apply interval: a target acknowledged early waits for min_apply_interval_ms on the
        // native clock (inclusive), one call per frame at most.
        {
            Rig s_Rig;
            Opened(s_Rig);
            s_Rig.Target(1, s_Rig.At(0.05, 0.0));
            s_Rig.Step(16);
            const uint64_t s_FirstCall = s_Rig.now;
            s_Rig.Target(2, s_Rig.At(0.1, 0.0));
            s_Rig.Step(16);
            s_Rig.Step(16);
            CHECK(s_Rig.applier.invokes == 1);
            s_Rig.now = s_FirstCall + 40 - 1;
            s_Rig.Step(0);
            CHECK(s_Rig.applier.invokes == 1);
            s_Rig.Step(1);
            CHECK(s_Rig.applier.invokes == 2 && LastAckOutcome(s_Rig) == "applied/");
        }
    }

    void Time()
    {
        // Target silence: open until opened + silence - 1; closed at exactly opened + silence, before
        // anything else; refreshed only by an accepted target, with its receipt time.
        {
            Rig s_Rig;
            Opened(s_Rig);
            const uint64_t s_Opened = s_Rig.now;
            s_Rig.now = s_Opened + 999;
            s_Rig.Step(0);
            CHECK(s_Rig.C().SessionOpen());
            s_Rig.Step(1);
            CHECK(!s_Rig.C().SessionOpen() && CloseReason(s_Rig) == "target_silence" && Field(s_Rig.Closeds().back(), "clean") == "true"
                  && Field(s_Rig.Closeds().back(), "final_world") == FloatWords::Encode(s_Rig.World()));
        }
        {
            Rig s_Rig;
            Opened(s_Rig);
            const uint64_t s_Opened = s_Rig.now;
            s_Rig.now = s_Opened + 600;
            s_Rig.Target(1, s_Rig.At(0.05, 0.0)); // received at opened + 600
            s_Rig.Step(16);
            CHECK(s_Rig.applier.invokes == 1);
            s_Rig.now = s_Opened + 600 + 999;
            s_Rig.Step(0);
            CHECK(s_Rig.C().SessionOpen());
            s_Rig.Step(1);
            CHECK(!s_Rig.C().SessionOpen() && CloseReason(s_Rig) == "target_silence");
        }

        // Duration: the frame at opened + duration - 1 still applies; the one at the bound closes.
        {
            Rig s_Rig;
            Opened(s_Rig, [](CommandProtocol::Command& c) { c.motion->limits.duration_ms = 1000; });
            const uint64_t s_Opened = s_Rig.now;
            s_Rig.now = s_Opened + 980;
            s_Rig.Target(1, s_Rig.At(0.05, 0.0));
            s_Rig.Step(19); // opened + 999
            CHECK(s_Rig.applier.invokes == 1 && s_Rig.C().SessionOpen());
            s_Rig.Step(1);
            CHECK(!s_Rig.C().SessionOpen() && CloseReason(s_Rig) == "duration");
        }

        // A target received BEFORE the duration deadline and processed at it: the deadline applies
        // first, the target is never applied (a late target of a closed session, never acknowledged
        // as applied, never reopening it).
        {
            Rig s_Rig;
            Opened(s_Rig, [](CommandProtocol::Command& c) { c.motion->limits.duration_ms = 1000; });
            const uint64_t s_Opened = s_Rig.now;
            s_Rig.now = s_Opened + 990;
            s_Rig.Step(0);
            s_Rig.now = s_Opened + 999;
            s_Rig.Target(1, s_Rig.At(0.05, 0.0)); // queued before the deadline
            s_Rig.Step(1);                         // processed at it
            CHECK(s_Rig.applier.invokes == 0 && !s_Rig.C().SessionOpen() && CloseReason(s_Rig) == "duration" && s_Rig.Acks().empty() && s_Rig.C().GetStats().late_targets == 1);
        }

        // A pending target still waiting (apply interval) when the deadline passes: refused,
        // session_closed, before ghost.closed; never invoked.
        {
            Rig s_Rig;
            Opened(s_Rig, [](CommandProtocol::Command& c) { c.motion->limits.duration_ms = 1000; });
            const uint64_t s_Opened = s_Rig.now;
            s_Rig.now = s_Opened + 950;
            s_Rig.Target(1, s_Rig.At(0.05, 0.0));
            s_Rig.Step(10); // 960: applied
            s_Rig.Target(2, s_Rig.At(0.1, 0.0));
            s_Rig.Step(16); // 976: waits (16 < 40)
            s_Rig.Step(16); // 992: waits (32 < 40)
            CHECK(s_Rig.applier.invokes == 1);
            s_Rig.Step(16); // 1008: the deadline first
            CHECK(s_Rig.applier.invokes == 1 && LastAckOutcome(s_Rig) == "refused/session_closed" && CloseReason(s_Rig) == "duration");
            const auto& s_Control = s_Rig.link.control;
            CHECK(!s_Control.empty() && Field(s_Control.back(), "kind") == "ghost.closed" && Field(s_Control[s_Control.size() - 2], "reason") == "session_closed");
        }

        // The required pre-call work (the checked log write) delayed across the deadline: the
        // re-check immediately before the call refuses it — no invocation after the bound.
        {
            Rig s_Rig;
            Opened(s_Rig, [](CommandProtocol::Command& c) { c.motion->limits.duration_ms = 1000; });
            const uint64_t s_Opened = s_Rig.now;
            s_Rig.now = s_Opened + 990;
            s_Rig.Target(1, s_Rig.At(0.05, 0.0));
            {
                MotionTest::LogHook::Scope s_Hook("applying target", [&] { s_Rig.now = s_Opened + 1000; });
                s_Rig.Step(5); // the frame at 995; the write ends at 1000
                CHECK(MotionTest::LogHook::hits == 1);
            }
            CHECK(s_Rig.applier.invokes == 0 && LastAckOutcome(s_Rig) == "refused/deadline_at_call:duration" && CloseReason(s_Rig) == "duration" && s_Rig.C().GetStats().deadline_at_call == 1);
        }
        {
            // the in-time control: the same write ending one millisecond before the bound
            Rig s_Rig;
            Opened(s_Rig, [](CommandProtocol::Command& c) { c.motion->limits.duration_ms = 1000; });
            const uint64_t s_Opened = s_Rig.now;
            s_Rig.now = s_Opened + 990;
            s_Rig.Target(1, s_Rig.At(0.05, 0.0));
            {
                MotionTest::LogHook::Scope s_Hook("applying target", [&] { s_Rig.now = s_Opened + 999; });
                s_Rig.Step(5);
            }
            CHECK(s_Rig.applier.invokes == 1 && LastAckOutcome(s_Rig) == "applied/");
            CHECK(Field(s_Rig.Acks().back(), "native_mono_ms") == std::to_string(s_Opened + 999)); // the call's own time
        }
        {
            // the silence crossed during the write
            Rig s_Rig;
            Opened(s_Rig);
            const uint64_t s_Opened = s_Rig.now;
            s_Rig.now = s_Opened + 990;
            s_Rig.link.Target(MotionTest::k_Session, 1, s_Rig.At(0.05, 0.0).first, s_Rig.At(0.05, 0.0).second, s_Opened + 10); // received long ago
            {
                MotionTest::LogHook::Scope s_Hook("applying target", [&] { s_Rig.now = s_Opened + 1010; });
                s_Rig.Step(5);
            }
            CHECK(s_Rig.applier.invokes == 0 && LastAckOutcome(s_Rig) == "refused/deadline_at_call:target_silence" && CloseReason(s_Rig) == "target_silence");
        }

        // A close that arrives DURING the pre-call write is honoured before the call.
        {
            Rig s_Rig;
            Opened(s_Rig);
            s_Rig.Target(1, s_Rig.At(0.05, 0.0));
            {
                MotionTest::LogHook::Scope s_Hook("applying target", [&] { s_Rig.link.Close(MotionTest::k_Session, "operator", s_Rig.now); });
                s_Rig.Step();
            }
            CHECK(s_Rig.applier.invokes == 0 && s_Rig.C().GetStats().cancelled_at_call == 1 && !s_Rig.C().SessionOpen() && CloseReason(s_Rig) == "beam:operator"
                  && LastAckOutcome(s_Rig) == "refused/session_closed");
        }
        {
            // a share.close arriving then (another stream) defers the target one frame, no more
            Rig s_Rig;
            Opened(s_Rig);
            s_Rig.Target(1, s_Rig.At(0.05, 0.0));
            {
                MotionTest::LogHook::Scope s_Hook("applying target", [&] { s_Rig.link.Close(MotionTest::k_Share, "operator", s_Rig.now, true); });
                s_Rig.Step();
            }
            CHECK(s_Rig.applier.invokes == 0 && s_Rig.C().SessionOpen());
            s_Rig.Step();
            CHECK(s_Rig.applier.invokes == 1 && LastAckOutcome(s_Rig) == "applied/");
        }

        // A close and a target in one drain, in either order.
        {
            Rig s_Rig;
            Opened(s_Rig);
            s_Rig.Target(1, s_Rig.At(0.05, 0.0));
            s_Rig.link.Close(MotionTest::k_Session, "operator", s_Rig.now);
            s_Rig.Step();
            CHECK(s_Rig.applier.invokes == 0 && LastAckOutcome(s_Rig) == "refused/session_closed" && CloseReason(s_Rig) == "beam:operator");
        }
        {
            Rig s_Rig;
            Opened(s_Rig);
            s_Rig.link.Close(MotionTest::k_Session, "operator", s_Rig.now);
            s_Rig.Target(1, s_Rig.At(0.05, 0.0));
            s_Rig.Step();
            CHECK(s_Rig.applier.invokes == 0 && s_Rig.Acks().empty() && s_Rig.C().GetStats().late_targets == 1);
        }

        // A stalled frame callback: nothing happens off the frame; on its return the deadlines apply
        // before the queued target.
        {
            Rig s_Rig;
            Opened(s_Rig);
            s_Rig.Target(1, s_Rig.At(0.05, 0.0));
            s_Rig.Step(1500);
            CHECK(s_Rig.applier.invokes == 0 && CloseReason(s_Rig) == "target_silence" && s_Rig.C().GetStats().late_targets == 1);
        }
    }

    void FrameChecks()
    {
        // Every pre-call refusal: no invocation, the session ends (a refusal is not uncertain).
        struct Case
        {
            const char* label;
            std::function<void(Rig&)> between; // applied after the open, before the target's frame
            const char* ack;
            const char* close;
        };
        const Case s_Cases[] = {
            {"untouched word (z)", [](Rig& r) { r.applier.world.world.w[11] = MotionTest::Word(MotionTest::Value(r.applier.world.world.w[11]) + 0.01f); }, "refused/untouched_word_changed", "refused:untouched_word_changed"},
            {"untouched word (axis)", [](Rig& r) { r.applier.world.world.w[0] = MotionTest::Word(0.9999999f); }, "refused/untouched_word_changed", "refused:untouched_word_changed"},
            {"unexplained horizontal change", [](Rig& r) { r.applier.world.world.w[9] = MotionTest::Word(MotionTest::Value(r.applier.world.world.w[9]) + 0.01f); }, "refused/position_changed", "refused:position_changed"},
            {"parent", [](Rig& r) { r.applier.world.parent = "feed000000000bad"; }, "refused/parent_changed", "refused:parent_changed"},
            {"dirty cache", [](Rig& r) { r.applier.world.dirty = true; }, "refused/snapshot_dirty", "refused:snapshot_dirty"},
            {"identity lost", [](Rig& r) { r.applier.world.resolved = false; }, "refused/identity:candidate_index_not_in_range", "refused:identity:candidate_index_not_in_range"},
            {"registry", [](Rig& r) { r.applier.world.in_registry = true; }, "refused/identity:candidate_in_registry", "refused:identity:candidate_in_registry"},
            {"local hero control unreadable", [](Rig& r) { r.applier.world.hero_readable = false; }, "refused/identity:hero_control_unavailable", "refused:identity:hero_control_unavailable"},
            {"another local hero", [](Rig& r) { r.applier.world.hero_id = "0123456789abcdef"; }, "refused/local_hero_changed", "refused:local_hero_changed"},
            {"fresh generation", [](Rig& r) { r.applier.world.fresh_generation = FakeTransform::k_Gen2; }, "refused/generation_changed", "refused:generation_changed"},
            {"latch", [](Rig& r) { r.applier.latched = true; }, "refused/effectful_latched", "refused:effectful_latched"},
            {"fault during the reads", [](Rig& r) { r.applier.world.mode = FakeTransform::Mode::ObserveFaults; }, "refused/fault_during_reads", "refused:fault_during_reads"},
        };
        for (const auto& s_Case : s_Cases)
        {
            Rig s_Rig;
            Opened(s_Rig);
            s_Case.between(s_Rig);
            s_Rig.Target(1, s_Rig.At(0.05, 0.0));
            s_Rig.Step();
            const bool s_Ok = s_Rig.applier.invokes == 0 && LastAckOutcome(s_Rig) == s_Case.ack && CloseReason(s_Rig) == s_Case.close && !s_Rig.C().SessionOpen();
            if (!s_Ok)
                std::printf("  frame check '%s': invokes %d ack '%s' close '%s'\n", s_Case.label, s_Rig.applier.invokes, LastAckOutcome(s_Rig).c_str(), CloseReason(s_Rig).c_str());
            CHECK(s_Ok);
        }

        // An identity loss also makes the final snapshot unqualified: the close is unclean (null).
        {
            Rig s_Rig;
            Opened(s_Rig);
            s_Rig.applier.world.resolved = false;
            s_Rig.Target(1, s_Rig.At(0.05, 0.0));
            s_Rig.Step();
            CHECK(Field(s_Rig.Closeds().back(), "clean") == "false" && Field(s_Rig.Closeds().back(), "final_world") == "");
            CHECK(s_Rig.C().Record() && s_Rig.C().Record()->invalidated && !s_Rig.C().Record()->restorable);
        }

        // The frame's transitions close the session on their own frame (with no target at all).
        struct Transition
        {
            const char* label;
            std::function<void(Rig&)> change;
            const char* close;
        };
        const Transition s_Transitions[] = {
            {"predicate fall", [](Rig& r) { r.playing = false; }, "mission_not_playing"},
            {"edge-cached generation", [](Rig& r) { r.generation = FakeTransform::k_Gen2; }, "generation_changed"},
            {"transition epoch", [](Rig& r) { ++r.epoch; }, "transition_epoch_changed"},
            {"scene", [](Rig& r) { r.scene = "assembly:/_PRO/Scenes/Frontend/MainMenu.entity"; }, "scene_changed"},
            {"command link", [](Rig& r) { r.command_gen = MotionTest::k_CommandGen + 1; }, "command_link_lost"},
            {"frame thread", [](Rig& r) { r.thread += 1; }, "frame_thread_changed"},
        };
        for (const auto& s_Transition : s_Transitions)
        {
            // a target applied, a second one pending behind the apply interval, then the transition:
            // the session closes on that frame and the pending target is refused, never invoked
            Rig s_Rig;
            Opened(s_Rig);
            s_Rig.Target(1, s_Rig.At(0.05, 0.0));
            s_Rig.Step();
            s_Rig.Target(2, s_Rig.At(0.1, 0.0));
            s_Rig.Step();
            s_Transition.change(s_Rig);
            s_Rig.Step();
            const bool s_Ok = s_Rig.applier.invokes == 1 && CloseReason(s_Rig) == s_Transition.close && !s_Rig.C().SessionOpen() && LastAckOutcome(s_Rig) == "refused/session_closed";
            if (!s_Ok)
                std::printf("  transition '%s': close '%s' ack '%s'\n", s_Transition.label, CloseReason(s_Rig).c_str(), LastAckOutcome(s_Rig).c_str());
            CHECK(s_Ok);
        }

        // The motion link's loss: the session closes autonomously; nothing can be sent.
        {
            Rig s_Rig;
            Opened(s_Rig);
            s_Rig.Target(1, s_Rig.At(0.05, 0.0));
            s_Rig.link.binding.bound = false;
            s_Rig.Step();
            CHECK(!s_Rig.C().SessionOpen() && s_Rig.applier.invokes == 0 && s_Rig.Closeds().empty() && s_Rig.C().GetStats().last_session_close == "motion_link_lost");
            // unreported: BEAM sees the loss (unknown) and never restores or releases it, so neither
            // does the native record (verifier F6; the draft kept it restorable)
            CHECK(s_Rig.C().Record() && !s_Rig.C().Record()->restorable && s_Rig.C().Record()->invalidated);
        }
        {
            // a reconnect is a new binding: the old session never resumes on it
            Rig s_Rig;
            Opened(s_Rig);
            s_Rig.link.binding.serial = 2;
            s_Rig.Target(1, s_Rig.At(0.05, 0.0));
            s_Rig.Step();
            CHECK(!s_Rig.C().SessionOpen() && s_Rig.applier.invokes == 0 && s_Rig.C().GetStats().last_session_close == "motion_link_lost");
        }
    }

    void PostCall()
    {
        // The first post-call result that is not the exact requested words is uncertain: it ends
        // application, the close is unclean and the record can never be restored or released.
        struct Case
        {
            FakeTransform::Mode mode;
            const char* reason;
        };
        const Case s_Cases[] = {
            {FakeTransform::Mode::Elsewhere, "postimage_mismatch"},
            {FakeTransform::Mode::AxesElsewhere, "postimage_mismatch"},
            {FakeTransform::Mode::Unchanged, "postimage_mismatch"},
            {FakeTransform::Mode::DirtyPost, "post_dirty"},
            {FakeTransform::Mode::ReadBackUnavailable, "read_back_unavailable"},
            {FakeTransform::Mode::PostEntityChange, "post_entity_changed"},
            {FakeTransform::Mode::PostGenerationChange, "generation_changed_after_call"},
            {FakeTransform::Mode::PostParentChange, "parent_changed_after_call"},
            {FakeTransform::Mode::ControlChanges, "hero_control_changed"},
            {FakeTransform::Mode::ControlDrifts, "hero_control_within_tolerance"},
            {FakeTransform::Mode::FaultDuringCall, "setter_fault"},
        };
        for (const auto& s_Case : s_Cases)
        {
            Rig s_Rig;
            Opened(s_Rig);
            s_Rig.Target(1, s_Rig.At(0.05, 0.0));
            s_Rig.Step();
            CHECK(s_Rig.applier.invokes == 1);
            s_Rig.applier.world.mode = s_Case.mode;
            s_Rig.Target(2, s_Rig.At(0.1, 0.0));
            s_Rig.Step(50);
            const bool s_Ok = s_Rig.applier.invokes == 2 && LastAckOutcome(s_Rig) == std::string("uncertain/") + s_Case.reason && CloseReason(s_Rig) == std::string("uncertain:") + s_Case.reason
                && Field(s_Rig.Closeds().back(), "clean") == "false" && Field(s_Rig.Closeds().back(), "applied") == "1" && Field(s_Rig.Closeds().back(), "last_applied_seq") == "1"
                && s_Rig.C().Record() && s_Rig.C().Record()->invalidated && s_Rig.C().GetStats().uncertain == 1 && s_Rig.C().GetStats().applied == 1;
            if (!s_Ok)
                std::printf("  post-call mode %d: invokes %d ack '%s' close '%s'\n", static_cast<int>(s_Case.mode), s_Rig.applier.invokes, LastAckOutcome(s_Rig).c_str(), CloseReason(s_Rig).c_str());
            CHECK(s_Ok);
            CHECK(Field(s_Rig.Acks().back(), "exact") == "false");
            // never restorable, never released, M6 held for the rest of the process
            CHECK(s_Rig.C().HoldsTransform(s_Rig.frame, nullptr));
        }

        // A fault latches the effectful path: no later session opens.
        {
            Rig s_Rig;
            Opened(s_Rig);
            s_Rig.applier.world.mode = FakeTransform::Mode::FaultDuringCall;
            s_Rig.Target(1, s_Rig.At(0.05, 0.0));
            s_Rig.Step();
            CHECK(s_Rig.applier.Latched());
        }

        // The checked line not accepted: no call; a refusal, not a latch; the close is clean.
        {
            Rig s_Rig;
            Opened(s_Rig);
            s_Rig.Target(1, s_Rig.At(0.05, 0.0));
            {
                MotionTest::LogHook::Scope s_Hook("applying target", nullptr, true);
                s_Rig.Step();
            }
            CHECK(s_Rig.applier.invokes == 0 && LastAckOutcome(s_Rig) == "refused/apply_log_unwritten:write_failed" && Field(s_Rig.Closeds().back(), "clean") == "true" && !s_Rig.applier.Latched());
        }
    }

    void Evidence()
    {
        // Required evidence that cannot be queued: no call at all (its acknowledgement could not be
        // reported — verifier F5 on the draft, which made the call and then dropped the link); the
        // binding is dropped and the session ends.
        {
            Rig s_Rig;
            Opened(s_Rig);
            s_Rig.Target(1, s_Rig.At(0.05, 0.0));
            s_Rig.link.control_room = 0;
            s_Rig.Step();
            CHECK(s_Rig.applier.invokes == 0 && !s_Rig.link.drops.empty() && s_Rig.C().GetStats().evidence_capacity == 1);
            CHECK(!s_Rig.C().SessionOpen() && s_Rig.C().GetStats().last_session_close == "evidence_undeliverable");
            s_Rig.Target(2, s_Rig.At(0.1, 0.0));
            s_Rig.Step(50);
            CHECK(s_Rig.applier.invokes == 0 && s_Rig.C().GetStats().late_targets == 1); // a closed session never applies
        }
        {
            // a report that cannot be queued ends the session at once
            Rig s_Rig;
            Opened(s_Rig);
            s_Rig.Target(1, s_Rig.At(0.05, 0.0));
            s_Rig.Step();
            s_Rig.link.control_room = 0;
            s_Rig.Step(300);
            CHECK(!s_Rig.C().SessionOpen() && s_Rig.C().GetStats().last_session_close == "evidence_undeliverable" && !s_Rig.link.drops.empty());
        }

        // Closes naming a closed session are harmless; a foreign close terminates nothing; nothing
        // reopens a closed session.
        {
            Rig s_Rig;
            Opened(s_Rig);
            s_Rig.link.Close(MotionTest::k_Foreign, "operator", s_Rig.now);
            s_Rig.Step();
            CHECK(s_Rig.C().SessionOpen() && s_Rig.C().GetStats().foreign_closes == 1);
            s_Rig.Step(1000);
            CHECK(!s_Rig.C().SessionOpen() && CloseReason(s_Rig) == "target_silence");
            const size_t s_Lines = s_Rig.link.control.size();
            s_Rig.link.Close(MotionTest::k_Session, "stall", s_Rig.now);
            s_Rig.Target(5, s_Rig.At(0.05, 0.0));
            s_Rig.Step();
            CHECK(s_Rig.link.control.size() == s_Lines && s_Rig.C().GetStats().closes_for_closed == 1 && s_Rig.C().GetStats().late_targets == 1 && !s_Rig.C().SessionOpen() && s_Rig.applier.invokes == 0);
        }
    }
}

void RunMotionSessionTests()
{
    Admission();
    Application();
    Time();
    FrameChecks();
    PostCall();
    Evidence();
}
