#include "TestHarness.h"

#include <thread>
#include <vector>

#include <Windows.h>

#include "CommandScheduler.h"
#include "TestListener.h"

using namespace CommandProtocol;

namespace
{
    Command Probe(const std::string& p_Id, uint64_t p_Generation = 1, uint32_t p_Ttl = 500)
    {
        Command s_Command;
        s_Command.command_id = p_Id;
        s_Command.link_generation = p_Generation;
        s_Command.ttl_ms = p_Ttl;
        s_Command.name = "relay.probe";
        s_Command.version = 1;
        s_Command.entry = Entry::RelayProbe1;
        return s_Command;
    }

    CommandScheduler::FrameObservation Frame(uint64_t p_Frame, uint64_t p_NowMs, uint64_t p_Generation, bool p_Available, bool p_Playing)
    {
        CommandScheduler::FrameObservation s_Frame;
        s_Frame.frame = p_Frame;
        s_Frame.now_mono_ms = p_NowMs;
        s_Frame.native_at = "t";
        s_Frame.thread_id = GetCurrentThreadId();
        s_Frame.scene_available = p_Available;
        s_Frame.scene.available = p_Available;
        s_Frame.scene.scene_loaded = p_Playing;
        s_Frame.scene.loading_stage = p_Playing ? 8 : 2;
        s_Frame.scene.scene_type = p_Playing ? "mission" : "";
        s_Frame.scene.codename_hint = p_Playing ? "Peacock" : "";
        s_Frame.mission_playing = p_Playing;
        if (p_Playing)
            s_Frame.mission_generation = "session-a";
        s_Frame.link_generation = p_Generation;
        s_Frame.adapter_instance_id = CommandTest::Uuid(99);
        return s_Frame;
    }

    const ObservedValue* Observed(const Result& p_Result, const char* p_Key)
    {
        for (const auto& s_Entry : p_Result.observed)
            if (s_Entry.first == p_Key)
                return &s_Entry.second;
        return nullptr;
    }
}

void RunCommandSchedulerTests()
{
    // One command per frame, in order, with the frame's read-back and no engine write.
    {
        CommandQueue s_Queue;
        std::vector<Result> s_Results;
        CommandScheduler s_Scheduler(s_Queue, [&](const Result& p_Result) { s_Results.push_back(p_Result); });

        s_Queue.Push(Probe(CommandTest::Uuid(1)), 1, 1000);
        s_Queue.Push(Probe(CommandTest::Uuid(2)), 1, 1000);

        CHECK(s_Scheduler.Tick(Frame(10, 1100, 1, true, true)));
        CHECK(s_Results.size() == 1);
        CHECK(s_Queue.GetStats().queued == 1);
        CHECK(s_Scheduler.Tick(Frame(11, 1120, 1, true, false)));
        CHECK(s_Results.size() == 2);
        CHECK(!s_Scheduler.Tick(Frame(12, 1140, 1, true, false)));

        const Result& s_First = s_Results[0];
        CHECK(s_First.status == Status::Applied && s_First.command_id == CommandTest::Uuid(1) && s_First.link_generation == 1);
        CHECK(s_First.frame && *s_First.frame == 10);
        CHECK(s_First.adapter_instance_id == CommandTest::Uuid(99));
        CHECK(s_First.mission_generation && *s_First.mission_generation == "session-a");
        CHECK(Observed(s_First, "mission_playing") && Observed(s_First, "mission_playing")->boolean);
        CHECK(Observed(s_First, "scene_loaded") && Observed(s_First, "scene_loaded")->boolean);
        CHECK(Observed(s_First, "loading_stage") && Observed(s_First, "loading_stage")->integer == 8);
        CHECK(Observed(s_First, "codename_hint") && Observed(s_First, "codename_hint")->text == "Peacock");
        CHECK(Observed(s_First, "receipt_to_frame_ms") && Observed(s_First, "receipt_to_frame_ms")->integer == 100);
        CHECK(s_First.observed.size() <= k_MaxObservedEntries);

        const Result& s_Second = s_Results[1];
        CHECK(s_Second.status == Status::Applied && !s_Second.mission_generation);
        CHECK(Observed(s_Second, "mission_playing") && !Observed(s_Second, "mission_playing")->boolean);

        const auto s_Stats = s_Scheduler.GetStats();
        CHECK(s_Stats.executed == 2 && s_Stats.frames == 3 && s_Stats.max_latency_ms == 120);
        CHECK(s_Stats.first_thread_id == GetCurrentThreadId() && !s_Stats.thread_changed);
    }

    // An unavailable scene: applied with the observation stated as unavailable, never guessed.
    {
        CommandQueue s_Queue;
        std::vector<Result> s_Results;
        CommandScheduler s_Scheduler(s_Queue, [&](const Result& p_Result) { s_Results.push_back(p_Result); });
        s_Queue.Push(Probe(CommandTest::Uuid(1)), 1, 0);
        CHECK(s_Scheduler.Tick(Frame(1, 10, 1, false, false)));
        CHECK(s_Results.size() == 1 && s_Results[0].status == Status::Applied);
        CHECK(Observed(s_Results[0], "scene_available") && !Observed(s_Results[0], "scene_available")->boolean);
        CHECK(!Observed(s_Results[0], "scene_loaded"));
        CHECK(s_Results[0].reason.find("unavailable") != std::string::npos);
    }

    // TTL on the native monotonic clock: a command older than its TTL at its scheduled frame expires unapplied.
    {
        CommandQueue s_Queue;
        std::vector<Result> s_Results;
        CommandScheduler s_Scheduler(s_Queue, [&](const Result& p_Result) { s_Results.push_back(p_Result); });
        s_Queue.Push(Probe(CommandTest::Uuid(1), 1, 100), 1, 1000);
        CHECK(s_Scheduler.Tick(Frame(1, 1101, 1, true, true)));
        CHECK(s_Results.size() == 1 && s_Results[0].status == Status::Expired);
        CHECK(s_Scheduler.GetStats().expired == 1 && s_Scheduler.GetStats().executed == 0);
        // Its id is remembered as expired: a resend is a duplicate, not a second attempt.
        CHECK(s_Queue.Push(Probe(CommandTest::Uuid(1), 1, 100), 1, 1200).outcome == CommandQueue::PushOutcome::DuplicateDone);
        // Exactly at the TTL is still in time.
        s_Queue.Push(Probe(CommandTest::Uuid(2), 1, 100), 1, 2000);
        CHECK(s_Scheduler.Tick(Frame(2, 2100, 1, true, true)));
        CHECK(s_Results.size() == 2 && s_Results[1].status == Status::Applied);
    }

    // Generation: a command from a link that is gone (or a mismatch) is dropped without a result.
    {
        CommandQueue s_Queue;
        std::vector<Result> s_Results;
        CommandScheduler s_Scheduler(s_Queue, [&](const Result& p_Result) { s_Results.push_back(p_Result); });
        s_Queue.Push(Probe(CommandTest::Uuid(1), 1), 1, 0);
        CHECK(s_Scheduler.Tick(Frame(1, 10, 0, true, true)));   // no link
        CHECK(s_Results.empty() && s_Scheduler.GetStats().stale_generation == 1);
        s_Queue.Push(Probe(CommandTest::Uuid(2), 1), 1, 0);
        CHECK(s_Scheduler.Tick(Frame(2, 20, 2, true, true)));   // a newer link
        CHECK(s_Results.empty() && s_Scheduler.GetStats().stale_generation == 2);
        CHECK(s_Queue.Push(Probe(CommandTest::Uuid(2), 2), 2, 30).outcome == CommandQueue::PushOutcome::DuplicateDone);
    }

    // A reserved or unknown entry that somehow reached the queue is rejected, never applied.
    {
        CommandQueue s_Queue;
        std::vector<Result> s_Results;
        CommandScheduler s_Scheduler(s_Queue, [&](const Result& p_Result) { s_Results.push_back(p_Result); });
        Command s_Outfit = Probe(CommandTest::Uuid(1));
        s_Outfit.name = "player.outfit.set";
        s_Outfit.entry = Entry::PlayerOutfitSet1Reserved;
        s_Queue.Push(s_Outfit, 1, 0);
        CHECK(s_Scheduler.Tick(Frame(1, 10, 1, true, true)));
        CHECK(s_Results.size() == 1 && s_Results[0].status == Status::Rejected && s_Scheduler.GetStats().rejected == 1);
    }

    // Thread separation: the scheduler records the thread that invokes it; a push from another
    // thread is only a queue operation and the frame thread stays the executing one.
    {
        CommandQueue s_Queue;
        std::vector<Result> s_Results;
        CommandScheduler s_Scheduler(s_Queue, [&](const Result& p_Result) { s_Results.push_back(p_Result); });
        std::thread s_Network([&] { s_Queue.Push(Probe(CommandTest::Uuid(1)), 1, 0); });
        s_Network.join();
        CHECK(s_Scheduler.Tick(Frame(1, 10, 1, true, true)));
        CHECK(s_Scheduler.GetStats().first_thread_id == GetCurrentThreadId());
        uint32_t s_Other = 0;
        std::thread s_Frame2([&] {
            s_Other = GetCurrentThreadId();
            s_Queue.Push(Probe(CommandTest::Uuid(2)), 1, 0);
            s_Scheduler.Tick(Frame(2, 20, 1, true, true));
        });
        s_Frame2.join();
        CHECK(s_Scheduler.GetStats().thread_changed && s_Scheduler.GetStats().last_thread_id == s_Other);
        CHECK(s_Results.size() == 2);
    }
}
