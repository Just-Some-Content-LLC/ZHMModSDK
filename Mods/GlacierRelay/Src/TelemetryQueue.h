#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "TelemetryObservation.h"

// Hands observations from the telemetry detour to the frame update. Bounded: when full, the
// newest observation is dropped and counted; the detour never blocks and never waits on anything
// but this mutex, which the drain side holds only for a swap. Both sides ran on the frame thread
// in every run so far, but nothing here assumes it.
class TelemetryQueue
{
public:
    explicit TelemetryQueue(size_t p_Capacity) : m_Capacity(p_Capacity) {}

    // Returns false (and counts a drop) when the queue is full.
    bool Push(TelemetryObservation p_Observation)
    {
        std::lock_guard<std::mutex> s_Lock(m_Mutex);

        if (m_Queue.size() >= m_Capacity)
        {
            ++m_Dropped;
            return false;
        }

        m_Queue.push_back(std::move(p_Observation));
        ++m_Pushed;
        return true;
    }

    // Takes everything queued, in order.
    std::vector<TelemetryObservation> Drain()
    {
        std::deque<TelemetryObservation> s_Taken;

        {
            std::lock_guard<std::mutex> s_Lock(m_Mutex);
            s_Taken.swap(m_Queue);
        }

        return std::vector<TelemetryObservation>(
            std::make_move_iterator(s_Taken.begin()), std::make_move_iterator(s_Taken.end())
        );
    }

    struct Stats
    {
        uint64_t pushed = 0;
        uint64_t dropped = 0;
        size_t queued = 0;
    };

    Stats GetStats() const
    {
        std::lock_guard<std::mutex> s_Lock(m_Mutex);
        return Stats{m_Pushed, m_Dropped, m_Queue.size()};
    }

    size_t Capacity() const { return m_Capacity; }

private:
    size_t m_Capacity;
    mutable std::mutex m_Mutex;
    std::deque<TelemetryObservation> m_Queue;
    uint64_t m_Pushed = 0;
    uint64_t m_Dropped = 0;
};
