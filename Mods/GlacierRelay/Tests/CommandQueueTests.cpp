#include "TestHarness.h"

#include "CommandQueue.h"
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
}

void RunCommandQueueTests()
{
    // Capacity: eight queued, the ninth is busy; nothing dropped.
    {
        CommandQueue s_Queue;
        for (unsigned i = 1; i <= 8; ++i)
            CHECK(s_Queue.Push(Probe(CommandTest::Uuid(i)), 1, 100 + i).outcome == CommandQueue::PushOutcome::Queued);
        CHECK(s_Queue.Push(Probe(CommandTest::Uuid(9)), 1, 200).outcome == CommandQueue::PushOutcome::Busy);
        auto s_Stats = s_Queue.GetStats();
        CHECK(s_Stats.queued == 8 && s_Stats.busy == 1 && s_Stats.pushed == 8);

        // Pop in order; the popped one is still remembered as not done until MarkDone.
        const auto s_First = s_Queue.Pop();
        CHECK(s_First && s_First->command.command_id == CommandTest::Uuid(1) && s_First->received_mono_ms == 101);
        CHECK(s_Queue.Push(Probe(CommandTest::Uuid(1)), 1, 300).outcome == CommandQueue::PushOutcome::DuplicateQueued);
        s_Queue.MarkDone(CommandTest::Uuid(1), 1, Status::Applied);
        const auto s_Again = s_Queue.Push(Probe(CommandTest::Uuid(1)), 1, 300);
        CHECK(s_Again.outcome == CommandQueue::PushOutcome::DuplicateDone && s_Again.prior_status == Status::Applied && s_Again.prior_generation == 1);
        CHECK(s_Queue.GetStats().duplicates == 2);
    }

    // Duplicate while queued, after completion, after reconnect (a later generation), and beyond the window.
    {
        CommandQueue s_Queue;
        CHECK(s_Queue.Push(Probe(CommandTest::Uuid(1)), 1, 0).outcome == CommandQueue::PushOutcome::Queued);
        CHECK(s_Queue.Push(Probe(CommandTest::Uuid(1), 1), 1, 1).outcome == CommandQueue::PushOutcome::DuplicateQueued);
        s_Queue.Pop();
        s_Queue.MarkDone(CommandTest::Uuid(1), 1, Status::Applied);
        const auto s_Reconnect = s_Queue.Push(Probe(CommandTest::Uuid(1), 2), 2, 2);
        CHECK(s_Reconnect.outcome == CommandQueue::PushOutcome::DuplicateDone && s_Reconnect.prior_generation == 1);

        // 64 further ids push the first out of the window: the same id is then accepted again.
        for (unsigned i = 100; i < 164; ++i)
        {
            s_Queue.Push(Probe(CommandTest::Uuid(i)), 2, 3);
            s_Queue.Pop();
            s_Queue.MarkDone(CommandTest::Uuid(i), 2, Status::Applied);
        }
        CHECK(s_Queue.GetStats().remembered == 64);
        CHECK(s_Queue.Push(Probe(CommandTest::Uuid(1), 2), 2, 4).outcome == CommandQueue::PushOutcome::Queued);
    }

    // Link loss: Clear invalidates everything waiting and remembers it as expired, so a resend is a duplicate.
    {
        CommandQueue s_Queue;
        s_Queue.Push(Probe(CommandTest::Uuid(1)), 1, 0);
        s_Queue.Push(Probe(CommandTest::Uuid(2)), 1, 0);
        const auto s_Dropped = s_Queue.Clear();
        CHECK(s_Dropped.size() == 2 && s_Queue.GetStats().queued == 0 && s_Queue.GetStats().cleared == 2);
        CHECK(!s_Queue.Pop());
        const auto s_Resend = s_Queue.Push(Probe(CommandTest::Uuid(1), 2), 2, 5);
        CHECK(s_Resend.outcome == CommandQueue::PushOutcome::DuplicateDone && s_Resend.prior_status == Status::Expired);
    }
}
