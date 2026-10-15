#include "TestHarness.h"

#include <chrono>
#include <string>
#include <vector>

#include "Attestation.h"
#include "CommandJson.h"
#include "CommandLink.h"
#include "CommandScheduler.h"
#include "FakeOutfitApplier.h"
#include "TestListener.h"

using namespace CommandProtocol;
using CommandTest::Listener;
using CommandTest::WaitUntil;

namespace
{
    uint64_t MonoMs()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    CommandLink::Options Fast(uint16_t p_Port)
    {
        CommandLink::Options s_Options;
        s_Options.port = p_Port;
        s_Options.connect_timeout_ms = 500;
        s_Options.welcome_timeout_ms = 1000;
        s_Options.backoff_initial_ms = 50;
        s_Options.backoff_max_ms = 200;
        return s_Options;
    }

    // Accepts the link's LIVE connection, reads its hello and answers the welcome, then waits
    // until the link reports the generation. On Windows loopback a connection the link already
    // abandoned (its welcome timed out while nobody accepted) can still sit in the backlog ahead
    // of the live attempt, so a dead one is closed and the next is accepted, a few times over.
    std::string Handshake(Listener& p_Listener, CommandLink& p_Link, uint64_t p_Generation)
    {
        for (int s_Attempt = 0; s_Attempt < 20; ++s_Attempt)
        {
            if (!p_Listener.Accept(3000))
                return "";

            const std::string s_Hello = p_Listener.ReadLine(1500);

            // A dead (abandoned) connection yields no hello: move on at once, so the listener
            // keeps up with the link's live attempt rather than falling behind its welcome timeout.
            if (!s_Hello.empty())
            {
                p_Listener.Send(CommandTest::Welcome(p_Generation));

                if (WaitUntil(1500, [&] { return p_Link.LinkGeneration() == p_Generation; }))
                    return s_Hello;
            }

            p_Listener.CloseClient();
        }

        return "";
    }

    std::string Field(const std::string& p_Line, const char* p_Key)
    {
        const auto s_Parsed = CommandJson::Parse(p_Line);
        if (!s_Parsed.ok)
            return "<unparsed>";
        const auto* s_Value = s_Parsed.value.Find(p_Key);
        if (!s_Value)
            return "<absent>";
        if (s_Value->kind == CommandJson::Value::Kind::String)
            return s_Value->text;
        if (s_Value->kind == CommandJson::Value::Kind::Number)
            return std::to_string(s_Value->integer);
        if (s_Value->kind == CommandJson::Value::Kind::Null)
            return "null";
        return "<other>";
    }

    CommandScheduler::FrameObservation Frame(uint64_t p_Frame, uint64_t p_Generation)
    {
        CommandScheduler::FrameObservation s_Frame;
        s_Frame.frame = p_Frame;
        s_Frame.now_mono_ms = MonoMs();
        s_Frame.native_at = "t";
        s_Frame.scene_available = true;
        s_Frame.scene.available = true;
        s_Frame.mission_playing = false;
        s_Frame.link_generation = p_Generation;
        s_Frame.adapter_instance_id = CommandTest::Uuid(99);
        return s_Frame;
    }
}

void RunCommandLinkTests()
{
    WSADATA s_Data;
    WSAStartup(MAKEWORD(2, 2), &s_Data);
    const uint16_t s_Port = CommandTest::FreePort();
    const std::string s_Adapter = CommandTest::Uuid(99);

    // 1. No listener: the link retries with backoff and never blocks anyone.
    {
        CommandQueue s_Queue;
        CommandLink s_Link(Fast(s_Port), s_Queue, s_Adapter, MonoMs);
        CHECK(WaitUntil(1500, [&] { return s_Link.GetStats().connect_attempts >= 3; }));
        CHECK(!s_Link.GetStats().connected && s_Link.LinkGeneration() == 0);
        CHECK(s_Link.GetStats().connect_attempts <= 12);
    }

    // 2. Handshake, commands, results, dedup, busy, TTL, generation, link loss, reconnect.
    {
        CommandQueue s_Queue;
        CommandLink s_Link(Fast(s_Port), s_Queue, s_Adapter, MonoMs);
        CommandScheduler s_Scheduler(s_Queue, [&](const Result& p_Result) { s_Link.PushResult(p_Result); });

        Listener s_Beam;
        CHECK(s_Beam.Start(s_Port));
        const std::string s_Hello = Handshake(s_Beam, s_Link, 1);
        CHECK(Field(s_Hello, "kind") == "hello");
        CHECK(Field(s_Hello, "adapter_instance_id") == s_Adapter);
        CHECK(Field(s_Hello, "command_protocol_version") == "1");
        CHECK(Field(s_Hello, "mission_generation") == "null");
        CHECK(WaitUntil(2000, [&] { return s_Link.LinkGeneration() == 1; }));
        CHECK(s_Link.GetStats().welcomed && s_Link.GetStats().network_thread_id != 0);

        // A valid probe: received at once, applied on the next frame.
        s_Beam.Send(CommandTest::Command(CommandTest::Uuid(1), 1));
        std::string s_Line = s_Beam.ReadLine(2000);
        CHECK(Field(s_Line, "status") == "received" && Field(s_Line, "command_id") == CommandTest::Uuid(1) && Field(s_Line, "link_generation") == "1");
        CHECK(Field(s_Line, "adapter_instance_id") == s_Adapter);
        CHECK(WaitUntil(1000, [&] { return s_Queue.GetStats().queued == 1; }));
        CHECK(s_Scheduler.Tick(Frame(5, s_Link.LinkGeneration())));
        s_Line = s_Beam.ReadLine(2000);
        CHECK(Field(s_Line, "status") == "applied" && Field(s_Line, "frame") == "5");

        // Duplicate after completion.
        s_Beam.Send(CommandTest::Command(CommandTest::Uuid(1), 1));
        s_Line = s_Beam.ReadLine(2000);
        CHECK(Field(s_Line, "status") == "duplicate");
        CHECK(!s_Scheduler.Tick(Frame(6, 1)));

        // Duplicate while queued: the second line is answered duplicate, the first still runs once.
        s_Beam.Send(CommandTest::Command(CommandTest::Uuid(2), 1));
        s_Beam.Send(CommandTest::Command(CommandTest::Uuid(2), 1));
        CHECK(Field(s_Beam.ReadLine(2000), "status") == "received");
        CHECK(Field(s_Beam.ReadLine(2000), "status") == "duplicate");
        CHECK(WaitUntil(1000, [&] { return s_Queue.GetStats().queued == 1; }));
        CHECK(s_Scheduler.Tick(Frame(7, 1)));
        CHECK(Field(s_Beam.ReadLine(2000), "status") == "applied");
        CHECK(!s_Scheduler.Tick(Frame(8, 1)));
        CHECK(s_Scheduler.GetStats().executed == 2);

        // Rejections that are answered: unknown command, reserved outfit command, unknown field,
        // a generation mismatch. None reaches the queue.
        s_Beam.Send(CommandTest::Command(CommandTest::Uuid(3), 1, 500, "relay.nope"));
        CHECK(Field(s_Beam.ReadLine(2000), "status") == "rejected");
        s_Beam.Send(CommandTest::Command(CommandTest::Uuid(4), 1, 500, "player.outfit.set", 1, "{\"disguise_repository_id\":\"9c8f3a75-7054-457c-919d-ee87e77f57d9\"}"));
        s_Line = s_Beam.ReadLine(2000);
        CHECK(Field(s_Line, "status") == "rejected" && Field(s_Line, "reason").find("reserved") != std::string::npos);
        s_Beam.Send(CommandTest::Command(CommandTest::Uuid(5), 2));
        s_Line = s_Beam.ReadLine(2000);
        CHECK(Field(s_Line, "status") == "rejected" && Field(s_Line, "reason").find("generation") != std::string::npos);
        CHECK(s_Queue.GetStats().queued == 0 && s_Link.GetStats().commands_rejected == 3);

        // Busy: nine queued probes, the ninth answered busy; nothing dropped silently.
        for (unsigned i = 10; i < 19; ++i)
            s_Beam.Send(CommandTest::Command(CommandTest::Uuid(i), 1));
        int s_Received = 0, s_Busy = 0;
        for (int i = 0; i < 9; ++i)
        {
            const std::string s_Status = Field(s_Beam.ReadLine(2000), "status");
            if (s_Status == "received") ++s_Received;
            if (s_Status == "busy") ++s_Busy;
        }
        CHECK(s_Received == 8 && s_Busy == 1);
        CHECK(s_Queue.GetStats().queued == 8);

        // Exactly one per frame, and a queue that expires before its frame: a 100 ms TTL probe
        // queued behind eight others while the frame source advances slowly.
        s_Beam.Send(CommandTest::Command(CommandTest::Uuid(20), 1, 100));
        CHECK(Field(s_Beam.ReadLine(2000), "status") == "busy");
        for (int i = 0; i < 8; ++i)
        {
            CHECK(s_Scheduler.Tick(Frame(100 + i, 1)));
            CHECK(Field(s_Beam.ReadLine(2000), "status") == "applied");
        }
        s_Beam.Send(CommandTest::Command(CommandTest::Uuid(21), 1, 100));
        CHECK(Field(s_Beam.ReadLine(2000), "status") == "received");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK(s_Scheduler.Tick(Frame(200, 1)));
        s_Line = s_Beam.ReadLine(2000);
        CHECK(Field(s_Line, "status") == "expired");
        CHECK(s_Scheduler.GetStats().expired == 1);

        // A violation closes the link (no answerable identity); the link reconnects with a new generation.
        s_Beam.Send("{not json\n");
        CHECK(s_Beam.PeerClosed(2000));
        // The counter moves before the disconnect resets the generation: wait for both.
        CHECK(WaitUntil(2000, [&] { return s_Link.GetStats().protocol_violations == 1 && s_Link.LinkGeneration() == 0; }));
        s_Beam.CloseClient();

        // Reconnect: queued work from the old link is gone; the welcome binds generation 2; a
        // command still naming generation 1 is rejected; an old id is a duplicate.
        CHECK(Handshake(s_Beam, s_Link, 2) != "");
        CHECK(WaitUntil(2000, [&] { return s_Link.LinkGeneration() == 2; }));
        s_Beam.Send(CommandTest::Command(CommandTest::Uuid(30), 1));
        CHECK(Field(s_Beam.ReadLine(2000), "status") == "rejected");
        s_Beam.Send(CommandTest::Command(CommandTest::Uuid(1), 2));
        s_Line = s_Beam.ReadLine(2000);
        CHECK(Field(s_Line, "status") == "duplicate" && Field(s_Line, "link_generation") == "2");

        // Link loss with a queued command: cleared, never applied, no result.
        s_Beam.Send(CommandTest::Command(CommandTest::Uuid(31), 2));
        CHECK(Field(s_Beam.ReadLine(2000), "status") == "received");
        CHECK(WaitUntil(1000, [&] { return s_Queue.GetStats().queued == 1; }));
        s_Beam.CloseClient();
        CHECK(WaitUntil(2000, [&] { return s_Link.GetStats().cleared_on_disconnect == 1; }));
        CHECK(s_Queue.GetStats().queued == 0 && s_Link.LinkGeneration() == 0);
        CHECK(!s_Scheduler.Tick(Frame(300, s_Link.LinkGeneration())));
        CHECK(s_Scheduler.GetStats().executed == 10);

        // An oversized line is a violation too.
        CHECK(Handshake(s_Beam, s_Link, 3) != "");
        CHECK(WaitUntil(2000, [&] { return s_Link.LinkGeneration() == 3; }));
        s_Beam.Send(std::string(4200, 'x') + "\n");
        CHECK(WaitUntil(2000, [&] { return s_Link.GetStats().protocol_violations == 2 && s_Link.LinkGeneration() == 0; }));

        // A refusal: BEAM says busy; the link backs off and retries later without a generation.
        s_Beam.CloseClient();
        bool s_Refused = false;
        for (int s_Attempt = 0; s_Attempt < 10 && !s_Refused; ++s_Attempt)
        {
            if (!s_Beam.Accept(3000))
                break;
            const std::string s_HelloLine = s_Beam.ReadLine(1500);
            if (!s_HelloLine.empty())
            {
                s_Beam.Send("{\"command_protocol_version\":1,\"kind\":\"refused\",\"reason\":\"busy\"}\n");
                s_Refused = WaitUntil(1500, [&] { return s_Link.GetStats().refusals >= 1; });
            }
            s_Beam.CloseClient();
        }
        CHECK(s_Refused);
        CHECK(s_Link.LinkGeneration() == 0);
        s_Beam.Stop();
    }

    // 3. Clean shutdown with and without a connection returns promptly; the thread ends.
    {
        const auto s_Start = std::chrono::steady_clock::now();
        {
            CommandQueue s_Queue;
            CommandLink s_Link(Fast(s_Port), s_Queue, s_Adapter, MonoMs);
        }
        CHECK(std::chrono::steady_clock::now() - s_Start < std::chrono::seconds(3));

        Listener s_Beam;
        CHECK(s_Beam.Start(s_Port));
        const auto s_Start2 = std::chrono::steady_clock::now();
        {
            CommandQueue s_Queue;
            CommandLink s_Link(Fast(s_Port), s_Queue, s_Adapter, MonoMs);
            CHECK(Handshake(s_Beam, s_Link, 1) != "");
            CHECK(WaitUntil(2000, [&] { return s_Link.LinkGeneration() == 1; }));
        }
        CHECK(std::chrono::steady_clock::now() - s_Start2 < std::chrono::seconds(3));
        CHECK(s_Beam.PeerClosed(2000));
        s_Beam.Stop();
    }

    // 4. M5 C-2B: a version-2 link (a test-only token): the hello says version 2 and advertises the
    //    effectful entry only when the setting is on; the attest covers the welcome's nonce, the
    //    generation and the adapter id with the shared MAC definition; a failed attestation (BEAM
    //    closing the link) leads to a fresh attest over the new welcome, never a reuse and never a
    //    fallback to version 1; the other version's welcome is rejected; effectful commands are
    //    decoded only on this link and one at a time.
    {
        const std::string s_Token = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        const std::string s_Nonce1 = "514f1d221aefccd9d7fc4193040369cf";
        const std::string s_Nonce2 = "a1b2c3d4e5f60718293a4b5c6d7e8f90";
        const std::string s_Suit = FakeOutfit::World::k_FakeSuit;
        const std::string s_Target = FakeOutfit::World::k_FakeTarget;

        CommandQueue s_Queue;
        CommandLink::Options s_Options = Fast(s_Port);
        s_Options.effectful = true;
        s_Options.token = s_Token;
        CommandLink s_Link(s_Options, s_Queue, s_Adapter, MonoMs);
        CHECK(s_Link.ProtocolVersion() == 2 && s_Link.GetStats().protocol_version == 2 && s_Link.GetStats().effectful);

        Listener s_Beam;
        CHECK(s_Beam.Start(s_Port));

        // Accept the live connection (skipping abandoned ones), read the hello, answer version 2.
        std::string s_Hello;
        for (int s_Attempt = 0; s_Attempt < 20 && s_Hello.empty(); ++s_Attempt)
        {
            if (!s_Beam.Accept(3000)) break;
            s_Hello = s_Beam.ReadLine(1500);
            if (s_Hello.empty()) s_Beam.CloseClient();
        }
        CHECK(Field(s_Hello, "command_protocol_version") == "2" && Field(s_Hello, "kind") == "hello");
        CHECK(s_Hello.find("\"player.outfit.set/2\"") != std::string::npos && s_Hello.find("\"relay.inspect.outfit/1\"") != std::string::npos);
        // M6-B1: the two read-only entries are claimed in the hello regardless of the effectful setting.
        CHECK(s_Hello.find("\"relay.inspect.players/1\"") != std::string::npos && s_Hello.find("\"relay.inspect.entity/1\"") != std::string::npos);
        CHECK(s_Hello.find(s_Token) == std::string::npos);

        s_Beam.Send(CommandTest::WelcomeV2(1, s_Nonce1));
        const std::string s_Attest = s_Beam.ReadLine(2000);
        CHECK(Field(s_Attest, "kind") == "attest" && Field(s_Attest, "command_protocol_version") == "2");
        CHECK(Field(s_Attest, "link_generation") == "1" && Field(s_Attest, "adapter_instance_id") == s_Adapter);
        CHECK(Field(s_Attest, "mac") == Attestation::AttestMac(s_Token, s_Nonce1, 1, s_Adapter));
        CHECK(WaitUntil(2000, [&] { return s_Link.LinkGeneration() == 1 && s_Link.GetStats().attests_sent == 1; }));

        // Commands and results carry version 2; a version-1 line is a violation that closes the link.
        s_Beam.Send(CommandTest::Command(CommandTest::Uuid(40), 1, 5000, "relay.probe", 1, "{}", "{}", 2));
        std::string s_Line = s_Beam.ReadLine(2000);
        CHECK(Field(s_Line, "status") == "received" && Field(s_Line, "command_protocol_version") == "2");

        // Two effectful commands: the first queued, the second busy; a v2 outfit command without
        // the effectful decode path would be rejected (checked on the inert link below).
        const std::string s_Params = CommandTest::OutfitParams(s_Target, 0, 0, s_Suit);
        s_Beam.Send(CommandTest::Command(CommandTest::Uuid(41), 1, 5000, "player.outfit.set", 2, s_Params, "{\"mission_generation\":\"gen-a\"}", 2));
        s_Beam.Send(CommandTest::Command(CommandTest::Uuid(42), 1, 5000, "player.outfit.set", 2, s_Params, "{\"mission_generation\":\"gen-a\"}", 2));
        CHECK(Field(s_Beam.ReadLine(2000), "status") == "received");
        s_Line = s_Beam.ReadLine(2000);
        CHECK(Field(s_Line, "status") == "busy" && Field(s_Line, "reason").find("effectful") != std::string::npos);
        CHECK(WaitUntil(1000, [&] { return s_Queue.GetStats().queued == 2; }) && s_Queue.HasPendingEffectful());
        // A malformed effectful command (the same tuple) is rejected before the queue.
        s_Beam.Send(CommandTest::Command(CommandTest::Uuid(43), 1, 5000, "player.outfit.set", 2, CommandTest::OutfitParams(s_Suit, 0, 0, s_Suit), "{\"mission_generation\":\"gen-a\"}", 2));
        s_Line = s_Beam.ReadLine(2000);
        CHECK(Field(s_Line, "status") == "rejected" && Field(s_Line, "reason").find("no-op") != std::string::npos);

        s_Beam.Send(CommandTest::Command(CommandTest::Uuid(44), 1));
        CHECK(s_Beam.PeerClosed(2000));
        CHECK(WaitUntil(2000, [&] { return s_Link.GetStats().protocol_violations == 1 && s_Link.LinkGeneration() == 0; }));
        CHECK(s_Queue.GetStats().queued == 0 && s_Link.GetStats().cleared_on_disconnect == 2);
        s_Beam.CloseClient();

        // Reconnect: a new generation and a new nonce get a new attest (different MAC); the earlier
        // attestation is never reused.
        s_Hello.clear();
        for (int s_Attempt = 0; s_Attempt < 20 && s_Hello.empty(); ++s_Attempt)
        {
            if (!s_Beam.Accept(3000)) break;
            s_Hello = s_Beam.ReadLine(1500);
            if (s_Hello.empty()) s_Beam.CloseClient();
        }
        CHECK(Field(s_Hello, "command_protocol_version") == "2");
        s_Beam.Send(CommandTest::WelcomeV2(2, s_Nonce2));
        const std::string s_Attest2 = s_Beam.ReadLine(2000);
        CHECK(Field(s_Attest2, "link_generation") == "2" && Field(s_Attest2, "mac") == Attestation::AttestMac(s_Token, s_Nonce2, 2, s_Adapter));
        CHECK(Field(s_Attest2, "mac") != Field(s_Attest, "mac"));
        CHECK(WaitUntil(2000, [&] { return s_Link.LinkGeneration() == 2 && s_Link.GetStats().attests_sent == 2; }));

        // An attestation failure as BEAM shows it (the link closed right after the attest): the
        // link reconnects and attests afresh, still version 2.
        s_Beam.CloseClient();
        CHECK(WaitUntil(2000, [&] { return s_Link.LinkGeneration() == 0; }));
        s_Hello.clear();
        for (int s_Attempt = 0; s_Attempt < 20 && s_Hello.empty(); ++s_Attempt)
        {
            if (!s_Beam.Accept(3000)) break;
            s_Hello = s_Beam.ReadLine(1500);
            if (s_Hello.empty()) s_Beam.CloseClient();
        }
        CHECK(Field(s_Hello, "command_protocol_version") == "2");

        // A version-1 welcome to the version-2 link: rejected; the link never binds a generation
        // and retries (no fallback).
        s_Beam.Send(CommandTest::Welcome(3));
        CHECK(s_Beam.PeerClosed(3000));
        CHECK(s_Link.LinkGeneration() == 0 && s_Link.GetStats().attests_sent == 2);
        s_Beam.CloseClient();
        s_Beam.Stop();
    }

    // 5. M5 C-2B: a version-1 link (no token) rejects a version-2 welcome and never decodes the
    //    effectful entry, whatever the setting says about it.
    {
        CommandQueue s_Queue;
        CommandLink::Options s_Options = Fast(s_Port);
        s_Options.effectful = true; // the setting alone does not make a version-1 link effectful-capable on the wire
        CommandLink s_Link(s_Options, s_Queue, s_Adapter, MonoMs);
        CHECK(s_Link.ProtocolVersion() == 1);
        Listener s_Beam;
        CHECK(s_Beam.Start(s_Port));
        std::string s_Hello;
        for (int s_Attempt = 0; s_Attempt < 20 && s_Hello.empty(); ++s_Attempt)
        {
            if (!s_Beam.Accept(3000)) break;
            s_Hello = s_Beam.ReadLine(1500);
            if (s_Hello.empty()) s_Beam.CloseClient();
        }
        CHECK(Field(s_Hello, "command_protocol_version") == "1");
        s_Beam.Send(CommandTest::WelcomeV2(1, "514f1d221aefccd9d7fc4193040369cf"));
        CHECK(s_Beam.PeerClosed(3000) && s_Link.LinkGeneration() == 0 && s_Link.GetStats().attests_sent == 0);
        s_Beam.CloseClient();

        CHECK(Handshake(s_Beam, s_Link, 2) != "");
        CHECK(WaitUntil(2000, [&] { return s_Link.LinkGeneration() == 2; }));
        // On a version-1 link the effectful entry is decoded only if the setting is on — and even
        // then the authority never advertises it to a version-1 peer; here it is simply the wire
        // check: a v2 line is a violation on a v1 link.
        s_Beam.Send(CommandTest::Command(CommandTest::Uuid(50), 2, 5000, "player.outfit.set", 2, CommandTest::OutfitParams(FakeOutfit::World::k_FakeTarget, 0, 0, FakeOutfit::World::k_FakeSuit), "{\"mission_generation\":\"g\"}", 2));
        CHECK(s_Beam.PeerClosed(2000));
        CHECK(WaitUntil(2000, [&] { return s_Link.LinkGeneration() == 0; }));
        s_Beam.Stop();

        // An invalid token is ignored: version 1, no attestation, and the token text is not logged
        // (checked by the absence of the attest; the log is not read here).
        CommandLink::Options s_Bad = Fast(s_Port);
        s_Bad.token = "not-a-token";
        CommandLink s_Link2(s_Bad, s_Queue, s_Adapter, MonoMs);
        CHECK(s_Link2.ProtocolVersion() == 1);
    }

    // 5. M5 pre-C-3 hardening (finding N4): bounded reconnect pacing. A link that BEAM welcomes and
    //    then closes at once (how a refused attestation shows) is retried after the backoff, which
    //    grows to its maximum, so a refusing BEAM sees a bounded number of connections; a link that
    //    stays welcomed for stable_ms resets the backoff; every retry is a fresh link with a new
    //    generation; a version-2 link refused three times recovers on the fourth welcome with a
    //    fresh attest each time and no reuse.
    {
        CommandQueue s_Queue;
        CommandLink::Options s_Options = Fast(s_Port); // backoff 50..200 ms
        s_Options.stable_ms = 300;
        CommandLink s_Link(s_Options, s_Queue, s_Adapter, MonoMs);
        Listener s_Beam;
        CHECK(s_Beam.Start(s_Port));

        // (a) 1.2 s of welcome-then-close: paced connects (≈ 50 + 100 + 200 + 200 + ... ms apart).
        const auto s_Start = std::chrono::steady_clock::now();
        uint64_t s_Generation = 0;
        int s_Welcomed = 0;
        while (std::chrono::steady_clock::now() - s_Start < std::chrono::milliseconds(1200))
        {
            if (!s_Beam.Accept(200))
                continue;
            const std::string s_Hello = s_Beam.ReadLine(300);
            if (!s_Hello.empty())
            {
                s_Beam.Send(CommandTest::Welcome(++s_Generation));
                ++s_Welcomed;
            }
            s_Beam.CloseClient();
        }
        auto s_Stats = s_Link.GetStats();
        std::printf("CommandLinkTests: pacing — %d welcomes closed at once in 1.2 s; link connects %llu, reconnect waits %llu, backoff now %u ms\n", s_Welcomed, s_Stats.connects, s_Stats.reconnect_waits, s_Stats.current_backoff_ms);
        CHECK(s_Welcomed >= 2 && s_Stats.connects >= 2 && s_Stats.connects <= 12);
        CHECK(s_Stats.reconnect_waits >= 2 && s_Stats.current_backoff_ms == s_Options.backoff_max_ms);
        CHECK(s_Stats.protocol_violations == 0 && s_Stats.protocol_version == 1);

        // (b) a welcomed link that stays up for stable_ms resets the backoff and keeps its generation.
        const std::string s_Hello = Handshake(s_Beam, s_Link, s_Generation + 1);
        CHECK(!s_Hello.empty() && s_Link.LinkGeneration() == s_Generation + 1);
        CHECK(WaitUntil(2000, [&] { return s_Link.GetStats().stable_links == 1; }));
        s_Stats = s_Link.GetStats();
        CHECK(s_Stats.current_backoff_ms == s_Options.backoff_initial_ms && s_Stats.welcomed);

        // (c) one loss after the stable link: the next reconnect waits the initial backoff once and
        //     comes back as the next generation.
        const uint64_t s_WaitsBefore = s_Stats.reconnect_waits;
        s_Beam.CloseClient();
        const std::string s_Hello2 = Handshake(s_Beam, s_Link, s_Generation + 2);
        CHECK(!s_Hello2.empty() && s_Link.LinkGeneration() == s_Generation + 2);
        CHECK(s_Link.GetStats().reconnect_waits == s_WaitsBefore + 1);
        s_Beam.Stop();
    }

    {
        // (d) version 2: three welcomes closed right after the attest (a refusing BEAM), then one
        //     kept: four fresh attests, four generations, paced retries, no downgrade, no reuse.
        const std::string s_Token = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        CommandQueue s_Queue;
        CommandLink::Options s_Options = Fast(s_Port);
        s_Options.token = s_Token;
        CommandLink s_Link(s_Options, s_Queue, s_Adapter, MonoMs);
        Listener s_Beam;
        CHECK(s_Beam.Start(s_Port));
        std::vector<std::string> s_Macs;

        for (uint64_t s_Gen = 1; s_Gen <= 4; ++s_Gen)
        {
            std::string s_Attest;
            for (int s_Attempt = 0; s_Attempt < 20 && s_Attest.empty(); ++s_Attempt)
            {
                if (!s_Beam.Accept(3000))
                    break;
                const std::string s_Hello = s_Beam.ReadLine(1500);
                if (s_Hello.empty()) { s_Beam.CloseClient(); continue; }
                CHECK(Field(s_Hello, "command_protocol_version") == "2");
                char s_Nonce[33];
                std::snprintf(s_Nonce, sizeof(s_Nonce), "%032llx", 0x1000000000000000ull + s_Gen);
                s_Beam.Send(CommandTest::WelcomeV2(s_Gen, s_Nonce));
                s_Attest = s_Beam.ReadLine(1500);
                if (s_Attest.empty()) s_Beam.CloseClient();
            }
            CHECK(!s_Attest.empty() && Field(s_Attest, "kind") == "attest" && Field(s_Attest, "link_generation") == std::to_string(s_Gen));
            s_Macs.push_back(Field(s_Attest, "mac"));
            if (s_Gen < 4)
                s_Beam.CloseClient(); // refused: BEAM closes after the attest
        }
        CHECK(WaitUntil(1500, [&] { return s_Link.LinkGeneration() == 4; }));
        const auto s_Stats = s_Link.GetStats();
        std::printf("CommandLinkTests: pacing v2 — attests %llu, connects %llu, reconnect waits %llu, generation %llu\n", s_Stats.attests_sent, s_Stats.connects, s_Stats.reconnect_waits, s_Stats.link_generation);
        CHECK(s_Stats.attests_sent == 4 && s_Stats.protocol_version == 2 && s_Stats.welcomed && s_Stats.reconnect_waits >= 3);
        CHECK(s_Macs.size() == 4 && s_Macs[0] != s_Macs[1] && s_Macs[1] != s_Macs[2] && s_Macs[2] != s_Macs[3]);
        s_Beam.Stop();
    }

    WSACleanup();
}
