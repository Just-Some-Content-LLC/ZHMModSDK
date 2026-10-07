#include "TestHarness.h"

#include <thread>

#include "TelemetryQueue.h"

namespace
{
    TelemetryObservation Observation(const char* p_Name, uint32_t p_Index)
    {
        TelemetryObservation s_Observation;
        s_Observation.name = p_Name;
        s_Observation.event_index = p_Index;
        return s_Observation;
    }
}

void RunTelemetryQueueTests()
{
    // Order and drain.
    {
        TelemetryQueue s_Queue(4);
        CHECK(s_Queue.Push(Observation("Kill", 1)));
        CHECK(s_Queue.Push(Observation("Pacify", 2)));
        CHECK(s_Queue.GetStats().queued == 2);

        const auto s_Drained = s_Queue.Drain();
        CHECK(s_Drained.size() == 2);
        CHECK(s_Drained[0].name == "Kill" && s_Drained[0].event_index == 1);
        CHECK(s_Drained[1].name == "Pacify" && s_Drained[1].event_index == 2);
        CHECK(s_Queue.GetStats().queued == 0);
        CHECK(s_Queue.Drain().empty());
    }

    // Overflow: the newest observation is dropped and counted; nothing blocks; earlier ones stay.
    {
        TelemetryQueue s_Queue(3);
        CHECK(s_Queue.Push(Observation("Kill", 1)));
        CHECK(s_Queue.Push(Observation("Kill", 2)));
        CHECK(s_Queue.Push(Observation("Kill", 3)));
        CHECK(!s_Queue.Push(Observation("Kill", 4)));
        CHECK(!s_Queue.Push(Observation("Kill", 5)));

        const auto s_Stats = s_Queue.GetStats();
        CHECK(s_Stats.pushed == 3 && s_Stats.dropped == 2 && s_Stats.queued == 3);

        const auto s_Drained = s_Queue.Drain();
        CHECK(s_Drained.size() == 3 && s_Drained[2].event_index == 3);

        // After a drain there is room again; the drop count is cumulative.
        CHECK(s_Queue.Push(Observation("Kill", 6)));
        CHECK(s_Queue.GetStats().dropped == 2);
    }

    // Pushing from another thread while draining does not lose or duplicate observations.
    {
        TelemetryQueue s_Queue(1000);
        std::thread s_Producer([&] {
            for (uint32_t i = 1; i <= 500; ++i)
                s_Queue.Push(Observation("Kill", i));
        });

        size_t s_Total = 0;
        uint32_t s_Last = 0;
        bool s_Ordered = true;

        while (s_Total < 500)
        {
            for (const auto& s_Observation : s_Queue.Drain())
            {
                s_Ordered = s_Ordered && s_Observation.event_index == s_Last + 1;
                s_Last = s_Observation.event_index;
                ++s_Total;
            }
        }

        s_Producer.join();
        CHECK(s_Total == 500 && s_Ordered);
        CHECK(s_Queue.GetStats().dropped == 0);
    }
}
