// M7-B2: the verifier's findings on the draft 41fbd800 as executable reproducers (each asserts the
// REQUIRED behaviour, so on the draft it fails and shows the actual one), with their in-time controls.
// F1 a target stamped at or after the silence deadline while the frame's time is earlier; F2 a
// share.closed that cannot be queued must stop the session's calls on that binding; F3 a close's final
// snapshot must be of the session's generation, parent and local hero; F4 the motion restore re-checks
// the command link after its checked line; F5 a call only when its evidence can be queued; F6 a record
// whose close was never reported is never restorable. FAKE link and FAKE engine only.

#include <chrono>
#include <cstdio>
#include <string>

#include "MotionTestRig.h"
#include "TestHarness.h"
#include "TestListener.h"

using CommandProtocol::Status;
using MotionTest::Field;
using MotionTest::Rig;

namespace
{
    std::string Ack(const Rig& p_Rig)
    {
        const auto s_Acks = p_Rig.Acks();
        return s_Acks.empty() ? std::string() : Field(s_Acks.back(), "outcome") + "/" + Field(s_Acks.back(), "reason");
    }

    void Opened(Rig& p_Rig)
    {
        p_Rig.Remember();
        CHECK(p_Rig.Open().status == Status::Applied);
    }

    void F1()
    {
        // The frame's time was taken before the drain (opened + 999); the link thread stamped the
        // target at the deadline (opened + 1000), and the clock reads that when the frame processes it.
        for (const uint64_t s_Stamp : {uint64_t(1000), uint64_t(1001)})
        {
            Rig s_Rig;
            Opened(s_Rig);
            const uint64_t s_Opened = s_Rig.now;
            s_Rig.now = s_Opened + s_Stamp;
            s_Rig.link.Target(MotionTest::k_Session, 1, s_Rig.At(0.05, 0.0).first, s_Rig.At(0.05, 0.0).second, s_Rig.now);
            ++s_Rig.frame;
            auto s_Frame = s_Rig.Frame();
            s_Frame.now_mono_ms = s_Opened + 999;
            s_Rig.C().Step(s_Frame);
            const bool s_Ok = s_Rig.applier.invokes == 0 && !s_Rig.C().SessionOpen() && s_Rig.C().GetStats().last_session_close == "target_silence";
            if (!s_Ok)
                std::printf("  F1 stamp +%llu: invokes %d open %d close '%s' ack '%s'\n", static_cast<unsigned long long>(s_Stamp), s_Rig.applier.invokes, s_Rig.C().SessionOpen(), s_Rig.C().GetStats().last_session_close.c_str(), Ack(s_Rig).c_str());
            CHECK(s_Ok);
        }
        // control: stamped one millisecond before the deadline, it is accepted and applied
        {
            Rig s_Rig;
            Opened(s_Rig);
            const uint64_t s_Opened = s_Rig.now;
            s_Rig.now = s_Opened + 999;
            s_Rig.link.Target(MotionTest::k_Session, 1, s_Rig.At(0.05, 0.0).first, s_Rig.At(0.05, 0.0).second, s_Rig.now);
            ++s_Rig.frame;
            auto s_Frame = s_Rig.Frame();
            s_Frame.now_mono_ms = s_Opened + 998;
            s_Rig.C().Step(s_Frame);
            CHECK(s_Rig.applier.invokes == 1 && s_Rig.C().SessionOpen() && Ack(s_Rig) == "applied/");
        }
    }

    void F2()
    {
        // A share and a session on one binding; the share's close cannot be queued (control full): the
        // binding is condemned — no setter call on it in that frame, the session ends, no new admission.
        Rig s_Rig;
        Opened(s_Rig);
        CHECK(s_Rig.OpenShare(50, 5000).status == Status::Applied);
        s_Rig.Target(1, s_Rig.At(0.05, 0.0));
        s_Rig.link.Close(MotionTest::k_Share, "operator", s_Rig.now, true);
        s_Rig.link.control_room = 0;
        s_Rig.Step();
        const bool s_Ok = s_Rig.applier.invokes == 0 && !s_Rig.C().SessionOpen() && !s_Rig.link.drops.empty();
        if (!s_Ok)
            std::printf("  F2: invokes %d session open %d drops %zu close '%s'\n", s_Rig.applier.invokes, s_Rig.C().SessionOpen(), s_Rig.link.drops.size(), s_Rig.C().GetStats().last_session_close.c_str());
        CHECK(s_Ok);
        // the condemned binding admits nothing new
        s_Rig.link.control_room = 1000;
        const auto s_Again = s_Rig.OpenShare(50, 5000, "5aa2e000-0000-4000-8000-000000000003");
        CHECK(s_Again.status != Status::Applied);
    }

    void F3()
    {
        // The close's final snapshot must be of the session's generation, its opening parent and its
        // local hero; otherwise it is unqualified (null) and the close unclean (never restorable).
        struct Case
        {
            const char* label;
            std::function<void(Rig&)> change;
        };
        const Case s_Cases[] = {
            {"fresh generation", [](Rig& r) { r.applier.world.fresh_generation = FakeTransform::k_Gen2; }},
            {"parent", [](Rig& r) { r.applier.world.parent = "feed000000000bad"; }},
            {"local hero", [](Rig& r) { r.applier.world.hero_id = "0123456789abcdef"; }},
        };
        for (const auto& s_Case : s_Cases)
        {
            Rig s_Rig;
            Opened(s_Rig);
            s_Case.change(s_Rig);
            s_Rig.Target(1, s_Rig.At(0.05, 0.0));
            s_Rig.Step();
            const auto s_Closed = s_Rig.Closeds();
            const bool s_Ok = s_Rig.applier.invokes == 0 && !s_Closed.empty() && Field(s_Closed.back(), "clean") == "false" && Field(s_Closed.back(), "final_world") == ""
                && s_Rig.C().Record() && s_Rig.C().Record()->invalidated;
            if (!s_Ok)
                std::printf("  F3 '%s': clean '%s' final '%s'\n", s_Case.label, s_Closed.empty() ? "?" : Field(s_Closed.back(), "clean").c_str(), s_Closed.empty() ? "?" : Field(s_Closed.back(), "final_world").c_str());
            CHECK(s_Ok);
        }
        // control: an unchanged world closes clean with its final words
        Rig s_Rig;
        Opened(s_Rig);
        s_Rig.link.Close(MotionTest::k_Session, "operator", s_Rig.now);
        s_Rig.Step();
        CHECK(Field(s_Rig.Closeds().back(), "clean") == "true" && Field(s_Rig.Closeds().back(), "final_world") == FloatWords::Encode(s_Rig.World()));
    }

    void DisplacedAndClosed(Rig& p_Rig)
    {
        Opened(p_Rig);
        p_Rig.Target(1, p_Rig.At(0.1, 0.0));
        p_Rig.Step();
        p_Rig.link.Close(MotionTest::k_Session, "operator", p_Rig.now);
        p_Rig.Step();
        CHECK(p_Rig.C().Record() && p_Rig.C().Record()->restorable);
    }

    void F4()
    {
        // The command link lost DURING the restore's checked line: no call (a proven non-application).
        Rig s_Rig;
        DisplacedAndClosed(s_Rig);
        CommandProtocol::Result s_Result;
        {
            MotionTest::LogHook::Scope s_Hook("applying candidate.motion.session/1 mode restore", [&] { s_Rig.command_gen = MotionTest::k_CommandGen + 1; });
            s_Result = s_Rig.Restore([&](CommandProtocol::Command& c) { c.motion->expect_world = s_Rig.World(); });
        }
        const bool s_Ok = s_Rig.applier.invokes == 1 && s_Result.status == Status::Unavailable && MotionTest::Observed(s_Result, "setter_called") == "false" && s_Rig.C().Record()->restorable;
        if (!s_Ok)
            std::printf("  F4: invokes %d status %s reason '%s'\n", s_Rig.applier.invokes, CommandProtocol::StatusText(s_Result.status), s_Result.reason.c_str());
        CHECK(s_Ok);
    }

    void F5()
    {
        // A call only when its consequences can be reported: the acknowledgement and a close.
        for (const size_t s_Room : {size_t(0), size_t(1), size_t(2)})
        {
            Rig s_Rig;
            Opened(s_Rig);
            s_Rig.Target(1, s_Rig.At(0.05, 0.0));
            s_Rig.link.control_room = s_Room;
            s_Rig.Step();
            const bool s_Ok = s_Room >= 2 ? s_Rig.applier.invokes == 1 : (s_Rig.applier.invokes == 0 && !s_Rig.C().SessionOpen());
            if (!s_Ok)
                std::printf("  F5 room %zu: invokes %d open %d\n", s_Room, s_Rig.applier.invokes, s_Rig.C().SessionOpen());
            CHECK(s_Ok);
        }
    }

    void F6()
    {
        // A close that could not be reported to BEAM (the link lost): BEAM never restores or releases
        // it, so the native record is not restorable either (and never released at baseline).
        Rig s_Rig;
        Opened(s_Rig);
        s_Rig.link.binding.bound = false;
        s_Rig.Step();
        const bool s_Ok = s_Rig.C().Record() && !s_Rig.C().Record()->restorable && s_Rig.C().Record()->invalidated;
        if (!s_Ok)
            std::printf("  F6: restorable %d invalidated %d\n", s_Rig.C().Record() ? s_Rig.C().Record()->restorable : -1, s_Rig.C().Record() ? s_Rig.C().Record()->invalidated : -1);
        CHECK(s_Ok);
        CHECK(s_Rig.C().HoldsTransform(s_Rig.frame, [&] { return s_Rig.applier.Current(false, s_Rig.applier.world.fresh_generation); }));
    }
}

namespace
{
    // F9: the motion link's reconnect backoff grows across bindings BEAM refuses after the attest, and
    // resets only after a binding stayed up for stable_ms (the command link's M5 N4 pacing).
    void F9()
    {
        WSADATA s_Data;
        WSAStartup(MAKEWORD(2, 2), &s_Data);
        CommandTest::Listener s_Beam;
        CHECK(s_Beam.Start(0));
        MotionLink::Options s_Options;
        s_Options.port = s_Beam.Port;
        s_Options.connect_timeout_ms = 500;
        s_Options.welcome_timeout_ms = 800;
        s_Options.backoff_initial_ms = 50;
        s_Options.backoff_max_ms = 800;
        s_Options.stable_ms = 300;
        s_Options.token = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        const auto s_Mono = [] { return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); };
        MotionLink s_Link(s_Options, "aaf540dd-c6df-4429-8fa5-84bc8933a13e", s_Mono, [] { return uint64_t(9); });
        auto s_Bind = [&](uint64_t p_Serial) {
            for (int s_Attempt = 0; s_Attempt < 20; ++s_Attempt)
            {
                if (!s_Beam.Accept(3000))
                    return false;
                if (!s_Beam.ReadLine(1500).empty())
                    break;
                s_Beam.CloseClient();
            }
            s_Beam.Send("{\"motion_protocol_version\":1,\"kind\":\"motion.welcome\",\"motion_link_generation\":4,\"nonce\":\"514f1d221aefccd9d7fc4193040369cf\"}\n");
            s_Beam.ReadLine(2000);
            return CommandTest::WaitUntil(2000, [&] { const auto b = s_Link.Current(); return b.bound && b.serial == p_Serial; });
        };
        // two bindings refused right after the attest: the backoff grows (never reset by a bind)
        for (uint64_t s_Serial = 1; s_Serial <= 2; ++s_Serial)
        {
            CHECK(s_Bind(s_Serial));
            s_Beam.Send("{\"motion_protocol_version\":1,\"kind\":\"motion.refused\",\"reason\":\"attest_failed\"}\n");
            CHECK(s_Beam.PeerClosed(2000));
            s_Beam.CloseClient();
        }
        CHECK(CommandTest::WaitUntil(2000, [&] { return s_Link.GetStats().current_backoff_ms >= 200; }) && s_Link.GetStats().stable_bindings == 0);
        // a binding that stays up for stable_ms resets it
        CHECK(s_Bind(3));
        CHECK(CommandTest::WaitUntil(2000, [&] { return s_Link.GetStats().stable_bindings == 1 && s_Link.GetStats().current_backoff_ms == 50; }));
        s_Beam.CloseClient();
        s_Beam.Stop();
        WSACleanup();
    }
}

void RunMotionVerifierTests()
{
    F9();
    F1();
    F2();
    F3();
    F4();
    F5();
    F6();
}
