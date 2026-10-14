#include "TestHarness.h"

#include <chrono>
#include <string>
#include <vector>

#include "CommandJson.h"
#include "CommandLink.h"
#include "CommandScheduler.h"
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

    WSACleanup();
}
