// M7-B2: the finite read-only share of the local hero (glacier-relay docs/design/M7_NETWORK_GHOST.md
// sections 15.4 and 15.6; 17.3), driven through Motion::Controller with the FAKE in-memory link and the
// FAKE world of the transform tests (the local hero's scripted words; never an engine read). The
// sampler reads the LOCAL hero only (registry slot 0), never the candidate and never anything a peer
// sent; it stops on its own; a missing read is never a clean zero.

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

    std::vector<std::string> Samples(const Rig& p_Rig) { return MotionTest::OfKind(p_Rig.link.samples, "hero.sample"); }
    std::vector<std::string> ShareClosed(const Rig& p_Rig) { return MotionTest::OfKind(p_Rig.link.control, "share.closed"); }

    void Open()
    {
        // The open: inert (no applier needed), on the bound link, the local hero's identity on the frame.
        {
            Rig s_Rig(false);
            const auto s_Result = s_Rig.OpenShare(50, 1000);
            CHECK(s_Result.status == Status::Applied && s_Rig.C().ShareOpen());
            CHECK(MotionTest::Observed(s_Result, "share_open") == "true" && MotionTest::Observed(s_Result, "hero_entity_id") == FakeTransform::k_HeroId);
            CHECK(MotionTest::Observed(s_Result, "interval_ms") == "50" && MotionTest::Observed(s_Result, "motion_link_generation") == "5");
            CHECK(s_Rig.applier.invokes == 0 && s_Rig.applier.observes == 0);
        }

        struct Case
        {
            const char* label;
            std::function<void(Rig&)> setup;
            Status status;
            const char* reason;
        };
        const Case s_Cases[] = {
            {"motion link unbound", [](Rig& r) { r.link.binding.bound = false; }, Status::Unavailable, "motion_link_not_bound"},
            {"motion link generation", [](Rig& r) { r.link.binding.motion_generation = 6; }, Status::PreconditionFailed, "motion_link_generation_mismatch"},
            {"hero unreadable", [](Rig& r) { r.applier.world.hero_readable = false; }, Status::Unavailable, "hero_identity_unavailable"},
            {"the candidate as the source", [](Rig& r) { r.applier.world.hero_id = FakeTransform::k_Entity; }, Status::PreconditionFailed, "hero_is_the_candidate"},
            {"the lookup returns the hero", [](Rig& r) { r.applier.world.distinct = false; }, Status::PreconditionFailed, "hero_is_the_candidate"},
            {"mission not playing", [](Rig& r) { r.playing = false; }, Status::PreconditionFailed, "mission_not_playing"},
            {"generation", [](Rig& r) { r.applier.world.fresh_generation = FakeTransform::k_Gen2; }, Status::PreconditionFailed, "generation_mismatch_fresh"},
        };
        for (const auto& s_Case : s_Cases)
        {
            Rig s_Rig(false);
            s_Case.setup(s_Rig);
            const auto s_Result = s_Rig.OpenShare();
            const bool s_Ok = s_Result.status == s_Case.status && Contains(s_Result.reason, s_Case.reason) && !s_Rig.C().ShareOpen();
            if (!s_Ok)
                std::printf("  share refusal '%s': status %s reason '%s'\n", s_Case.label, CommandProtocol::StatusText(s_Result.status), s_Result.reason.c_str());
            CHECK(s_Ok);
        }

        // One share at a time; an id is never reused.
        {
            Rig s_Rig(false);
            CHECK(s_Rig.OpenShare().status == Status::Applied);
            const auto s_Second = s_Rig.OpenShare(50, 1000, "5aa2e000-0000-4000-8000-000000000002");
            CHECK(s_Second.status == Status::PreconditionFailed && Contains(s_Second.reason, "share_open"));
            s_Rig.link.Close(MotionTest::k_Share, "operator", s_Rig.now, true);
            s_Rig.Step();
            CHECK(!s_Rig.C().ShareOpen());
            CHECK(Contains(s_Rig.OpenShare().reason, "share_id_reused"));
        }
    }

    void Sampling()
    {
        // The cadence: one sample per interval on the native clock (the first one interval after the
        // open), each with the frame's own frame number and native time, the hero's x, y, z words and
        // its dirty flag; the duration ends it on its own and share.closed names what was queued.
        {
            Rig s_Rig(false);
            CHECK(s_Rig.OpenShare(50, 1000).status == Status::Applied);
            const uint64_t s_Opened = s_Rig.now;
            for (int i = 0; i < 70; ++i)
            {
                s_Rig.applier.world.hero_world.w[9] = MotionTest::Word(MotionTest::Value(s_Rig.applier.world.hero_world.w[9]) + 0.01f); // the FAKE hero moves
                s_Rig.Step(16);
            }
            const auto s_Samples = Samples(s_Rig);
            CHECK(!s_Rig.C().ShareOpen() && s_Rig.C().GetStats().last_share_close == "duration");
            // ticks at 50 ms on a 16 ms frame clock: the first at >= opened + 50, each >= 50 ms after the last
            bool s_Spaced = !s_Samples.empty();
            uint64_t s_Last = 0;
            for (size_t i = 0; i < s_Samples.size(); ++i)
            {
                const uint64_t s_At = std::stoull(Field(s_Samples[i], "native_mono_ms"));
                if (Field(s_Samples[i], "seq") != std::to_string(i + 1) || s_At >= s_Opened + 1000 || (i == 0 ? s_At < s_Opened + 50 : s_At < s_Last + 50) || Field(s_Samples[i], "dirty") != "false")
                    s_Spaced = false;
                s_Last = s_At;
            }
            CHECK(s_Spaced && s_Samples.size() >= 15 && s_Samples.size() <= 20);
            const auto s_Closed = ShareClosed(s_Rig);
            CHECK(s_Closed.size() == 1);
            if (!s_Closed.empty())
                CHECK(Field(s_Closed[0], "reason") == "duration" && Field(s_Closed[0], "sent") == std::to_string(s_Samples.size()) && Field(s_Closed[0], "last_seq") == std::to_string(s_Samples.size()));
            // the pos words are the hero's, never the candidate's
            if (!s_Samples.empty())
            {
                const std::string s_Pos = Field(s_Samples.back(), "pos");
                CHECK(Contains(s_Pos, FloatWords::Hex8(s_Rig.applier.world.hero_world.w[10])) && !Contains(s_Pos, FloatWords::Hex8(s_Rig.applier.world.world.w[9])));
            }
            CHECK(s_Rig.applier.invokes == 0 && s_Rig.applier.observes == 0);
        }

        // No catch-up after a slow frame: one sample, then the next one interval later.
        {
            Rig s_Rig(false);
            CHECK(s_Rig.OpenShare(50, 5000).status == Status::Applied);
            s_Rig.Step(60);
            CHECK(Samples(s_Rig).size() == 1);
            s_Rig.Step(400);
            CHECK(Samples(s_Rig).size() == 2);
            s_Rig.Step(16);
            CHECK(Samples(s_Rig).size() == 2);
        }

        // A dirty read is sent labelled dirty; an unreadable one is no line (a gap in the sequence),
        // never a zero; a full sample queue refuses the newest (counted), nothing older is replaced.
        {
            Rig s_Rig(false);
            CHECK(s_Rig.OpenShare(50, 5000).status == Status::Applied);
            s_Rig.Step(50); // seq 1, clean
            s_Rig.applier.world.hero_dirty = true;
            s_Rig.Step(50); // seq 2, dirty
            s_Rig.applier.world.hero_dirty = false;
            s_Rig.applier.world.hero_transform_readable = false;
            s_Rig.Step(50); // seq 3: identity readable, transform not: no line
            s_Rig.applier.world.hero_transform_readable = true;
            s_Rig.Step(50); // seq 4, clean
            const auto s_Samples = Samples(s_Rig);
            CHECK(s_Samples.size() == 3 && s_Rig.C().GetStats().samples_unavailable == 1 && s_Rig.C().GetStats().samples_dirty == 1);
            if (s_Samples.size() == 3)
                CHECK(Field(s_Samples[0], "seq") == "1" && Field(s_Samples[1], "seq") == "2" && Field(s_Samples[1], "dirty") == "true" && Field(s_Samples[2], "seq") == "4" && Field(s_Samples[2], "dirty") == "false");
            s_Rig.link.sample_room = 0;
            s_Rig.Step(50); // seq 5: refused by the full queue
            CHECK(Samples(s_Rig).size() == 3 && s_Rig.C().GetStats().samples_dropped == 1 && s_Rig.C().ShareOpen());
            s_Rig.link.Close(MotionTest::k_Share, "operator", s_Rig.now, true);
            s_Rig.Step();
            const auto s_Closed = ShareClosed(s_Rig);
            CHECK(s_Closed.size() == 1);
            if (!s_Closed.empty())
                CHECK(Field(s_Closed[0], "sent") == "3" && Field(s_Closed[0], "last_seq") == "4" && Field(s_Closed[0], "reason") == "beam:operator");
        }

        // A close with no samples: last_seq null, sent 0.
        {
            Rig s_Rig(false);
            CHECK(s_Rig.OpenShare(50, 5000).status == Status::Applied);
            s_Rig.link.Close(MotionTest::k_Share, "operator", s_Rig.now, true);
            s_Rig.Step(10);
            const auto s_Closed = ShareClosed(s_Rig);
            CHECK(s_Closed.size() == 1 && Field(s_Closed[0], "last_seq") == "" && Field(s_Closed[0], "sent") == "0" && Samples(s_Rig).empty());
        }

        // A foreign share.close terminates nothing.
        {
            Rig s_Rig(false);
            CHECK(s_Rig.OpenShare(50, 5000).status == Status::Applied);
            s_Rig.link.Close("5aa2e000-0000-4000-8000-0000000000ff", "operator", s_Rig.now, true);
            s_Rig.Step(10);
            CHECK(s_Rig.C().ShareOpen() && s_Rig.C().GetStats().foreign_closes == 1);
        }
    }

    void AutonomousStops()
    {
        struct Case
        {
            const char* label;
            std::function<void(Rig&)> change;
            const char* reason;
            bool sent;
        };
        const Case s_Cases[] = {
            {"predicate fall", [](Rig& r) { r.playing = false; }, "mission_not_playing", true},
            {"edge-cached generation", [](Rig& r) { r.generation = FakeTransform::k_Gen2; }, "generation_changed", true},
            {"fresh generation", [](Rig& r) { r.applier.world.fresh_generation = FakeTransform::k_Gen2; }, "generation_changed", true},
            {"transition epoch", [](Rig& r) { ++r.epoch; }, "transition_epoch_changed", true},
            {"local hero identity", [](Rig& r) { r.applier.world.hero_id = "0123456789abcdef"; }, "hero_identity_lost", true},
            {"the hero became the candidate", [](Rig& r) { r.applier.world.distinct = false; }, "hero_is_the_candidate", true},
            {"command link", [](Rig& r) { r.command_gen = MotionTest::k_CommandGen + 1; }, "command_link_lost", true},
            {"motion link", [](Rig& r) { r.link.binding.bound = false; }, "motion_link_lost", false},
        };
        for (const auto& s_Case : s_Cases)
        {
            Rig s_Rig(false);
            CHECK(s_Rig.OpenShare(50, 5000).status == Status::Applied);
            s_Rig.Step(50);
            s_Case.change(s_Rig);
            s_Rig.Step(50);
            const auto s_Closed = ShareClosed(s_Rig);
            const bool s_Ok = !s_Rig.C().ShareOpen() && s_Rig.C().GetStats().last_share_close == s_Case.reason && (s_Closed.size() == 1) == s_Case.sent && Samples(s_Rig).size() == 1;
            if (!s_Ok)
                std::printf("  share stop '%s': open %d close '%s' closed lines %zu samples %zu\n", s_Case.label, s_Rig.C().ShareOpen(), s_Rig.C().GetStats().last_share_close.c_str(), s_Closed.size(), Samples(s_Rig).size());
            CHECK(s_Ok);
        }

        // A message cannot extend a share: a target or a session close naming it changes nothing.
        {
            Rig s_Rig(false);
            CHECK(s_Rig.OpenShare(50, 1000).status == Status::Applied);
            const uint64_t s_Opened = s_Rig.now;
            s_Rig.link.Target(MotionTest::k_Share, 1, 0, 0, s_Rig.now);
            s_Rig.link.Close(MotionTest::k_Share, "extend", s_Rig.now);
            s_Rig.now = s_Opened + 999;
            s_Rig.Step(0);
            CHECK(s_Rig.C().ShareOpen());
            s_Rig.Step(1);
            CHECK(!s_Rig.C().ShareOpen() && s_Rig.C().GetStats().last_share_close == "duration");
        }
    }
}

void RunMotionShareTests()
{
    Open();
    Sampling();
    AutonomousStops();
}
