#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "CommandProtocol.h"

// The bounded command queue and the recent-id registry (M5-B). The network thread validates and
// pushes; the frame-update scheduler pops at most one per frame. Capacity eight: a push into a
// full queue is Busy and nothing is dropped silently. A duplicate command id is detected against
// a bounded ring of the last 64 ids with their last known state, so relay.probe never executes
// twice for one id while that id is remembered — and no further: the ring is a bounded cache,
// not an exactly-once guarantee for all time (an id older than the window is not recognized).
// Every receipt is stamped with the native monotonic clock (injected, so tests control it); TTL
// is judged against that clock only, never against the issuer's wall clock.
class CommandQueue
{
public:
    static constexpr size_t k_Capacity = 8;
    static constexpr size_t k_DedupWindow = 64;

    struct Pending
    {
        CommandProtocol::Command command;
        uint64_t received_mono_ms = 0;
        uint64_t link_generation = 0; // the generation the link held when the command arrived
    };

    enum class PushOutcome
    {
        Queued,
        Busy,           // capacity exhausted
        DuplicateQueued, // same id already waiting
        DuplicateDone,   // same id already reached a terminal state (any link)
    };

    struct PushResult
    {
        PushOutcome outcome = PushOutcome::Queued;
        CommandProtocol::Status prior_status = CommandProtocol::Status::Rejected; // DuplicateDone: what it reached
        uint64_t prior_generation = 0;                                             // DuplicateDone/Queued: the link it came on
    };

    struct Stats
    {
        uint64_t pushed = 0;
        uint64_t busy = 0;
        uint64_t duplicates = 0;
        uint64_t popped = 0;
        uint64_t cleared = 0;
        size_t queued = 0;
        size_t remembered = 0;
    };

    PushResult Push(const CommandProtocol::Command& p_Command, uint64_t p_LinkGeneration, uint64_t p_NowMs)
    {
        std::lock_guard<std::mutex> s_Lock(m_Mutex);
        PushResult s_Result;

        for (const auto& s_Known : m_Recent)
        {
            if (s_Known.command_id != p_Command.command_id)
                continue;

            s_Result.prior_generation = s_Known.link_generation;

            if (s_Known.done)
            {
                s_Result.outcome = PushOutcome::DuplicateDone;
                s_Result.prior_status = s_Known.status;
            }
            else
            {
                s_Result.outcome = PushOutcome::DuplicateQueued;
            }

            ++m_Stats.duplicates;
            return s_Result;
        }

        if (m_Queue.size() >= k_Capacity)
        {
            s_Result.outcome = PushOutcome::Busy;
            ++m_Stats.busy;
            return s_Result;
        }

        m_Queue.push_back(Pending{p_Command, p_NowMs, p_LinkGeneration});
        Remember(Known{p_Command.command_id, p_LinkGeneration, false, CommandProtocol::Status::Received});
        ++m_Stats.pushed;
        return s_Result;
    }

    // The scheduler's one command for this frame, if any.
    std::optional<Pending> Pop()
    {
        std::lock_guard<std::mutex> s_Lock(m_Mutex);

        if (m_Queue.empty())
            return std::nullopt;

        Pending s_Pending = std::move(m_Queue.front());
        m_Queue.pop_front();
        ++m_Stats.popped;
        return s_Pending;
    }

    // Records the terminal state of an id so a later duplicate is answered as such.
    void MarkDone(const std::string& p_CommandId, uint64_t p_LinkGeneration, CommandProtocol::Status p_Status)
    {
        std::lock_guard<std::mutex> s_Lock(m_Mutex);

        for (auto& s_Known : m_Recent)
        {
            if (s_Known.command_id == p_CommandId)
            {
                s_Known.done = true;
                s_Known.status = p_Status;
                s_Known.link_generation = p_LinkGeneration;
                return;
            }
        }

        Remember(Known{p_CommandId, p_LinkGeneration, true, p_Status});
    }

    // Link loss: everything waiting is invalid (its generation is gone). Returns the ids dropped;
    // their ring entries are marked done as Expired so a resend on a new link is a duplicate of
    // something that never ran, which the scheduler never sees.
    std::vector<std::string> Clear()
    {
        std::lock_guard<std::mutex> s_Lock(m_Mutex);
        std::vector<std::string> s_Dropped;

        for (const auto& s_Pending : m_Queue)
        {
            s_Dropped.push_back(s_Pending.command.command_id);
            for (auto& s_Known : m_Recent)
                if (s_Known.command_id == s_Pending.command.command_id && !s_Known.done)
                {
                    s_Known.done = true;
                    s_Known.status = CommandProtocol::Status::Expired;
                }
        }

        m_Stats.cleared += m_Queue.size();
        m_Queue.clear();
        return s_Dropped;
    }

    Stats GetStats() const
    {
        std::lock_guard<std::mutex> s_Lock(m_Mutex);
        Stats s_Stats = m_Stats;
        s_Stats.queued = m_Queue.size();
        s_Stats.remembered = m_Recent.size();
        return s_Stats;
    }

private:
    struct Known
    {
        std::string command_id;
        uint64_t link_generation;
        bool done;
        CommandProtocol::Status status;
    };

    void Remember(Known p_Known)
    {
        if (m_Recent.size() >= k_DedupWindow)
            m_Recent.pop_front();
        m_Recent.push_back(std::move(p_Known));
    }

private:
    mutable std::mutex m_Mutex;
    std::deque<Pending> m_Queue;
    std::deque<Known> m_Recent;
    Stats m_Stats;
};
