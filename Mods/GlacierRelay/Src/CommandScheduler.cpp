#include "CommandScheduler.h"

#include "RelayLog.h"

using CommandProtocol::ObservedValue;
using CommandProtocol::Result;
using CommandProtocol::Status;

namespace
{
    ObservedValue Bool(bool p_Value)
    {
        ObservedValue s_Value;
        s_Value.kind = ObservedValue::Kind::Bool;
        s_Value.boolean = p_Value;
        return s_Value;
    }

    ObservedValue Integer(int64_t p_Value)
    {
        ObservedValue s_Value;
        s_Value.kind = ObservedValue::Kind::Integer;
        s_Value.integer = p_Value;
        return s_Value;
    }

    ObservedValue Text(std::string p_Value)
    {
        ObservedValue s_Value;
        s_Value.kind = ObservedValue::Kind::Text;
        s_Value.text = std::move(p_Value);
        return s_Value;
    }
}

Result CommandScheduler::Base(const CommandQueue::Pending& p_Pending, const FrameObservation& p_Frame) const
{
    Result s_Result;
    s_Result.command_id = p_Pending.command.command_id;
    s_Result.link_generation = p_Pending.link_generation;
    s_Result.adapter_instance_id = p_Frame.adapter_instance_id;
    s_Result.native_at = p_Frame.native_at;
    s_Result.frame = p_Frame.frame;
    s_Result.mission_generation = p_Frame.mission_generation;
    return s_Result;
}

bool CommandScheduler::Tick(const FrameObservation& p_Frame)
{
    ++m_Stats.frames;

    if (m_Stats.first_thread_id == 0)
        m_Stats.first_thread_id = p_Frame.thread_id;
    else if (p_Frame.thread_id != m_Stats.first_thread_id)
        m_Stats.thread_changed = true;

    m_Stats.last_thread_id = p_Frame.thread_id;

    const auto s_Pending = m_Queue.Pop();

    if (!s_Pending)
        return false;

    const auto& s_Command = s_Pending->command;

    // A command from a link that no longer exists: its issuer has already been told the link
    // was lost (and marked it unknown); there is nobody to answer and nothing to apply.
    if (p_Frame.link_generation == 0 || s_Pending->link_generation != p_Frame.link_generation)
    {
        ++m_Stats.stale_generation;
        m_Queue.MarkDone(s_Command.command_id, s_Pending->link_generation, Status::Expired);
        RelayLog::Warn("command scheduler: dropped {} from link generation {} (current {})", s_Command.command_id, s_Pending->link_generation, p_Frame.link_generation);
        return true;
    }

    Result s_Result = Base(*s_Pending, p_Frame);
    const uint64_t s_Age = p_Frame.now_mono_ms >= s_Pending->received_mono_ms ? p_Frame.now_mono_ms - s_Pending->received_mono_ms : 0;

    if (s_Age > s_Command.ttl_ms)
    {
        ++m_Stats.expired;
        s_Result.status = Status::Expired;
        s_Result.reason = "ttl elapsed before the scheduled frame (" + std::to_string(s_Age) + " ms on the native clock)";
        m_Queue.MarkDone(s_Command.command_id, s_Pending->link_generation, Status::Expired);
        m_Emit(s_Result);
        return true;
    }

    switch (s_Command.entry)
    {
        case CommandProtocol::Entry::RelayProbe1:
        {
            // No engine write. The read-back is what this frame observed, as plain values.
            ++m_Stats.executed;
            m_Stats.last_executed_frame = p_Frame.frame;
            m_Stats.last_latency_ms = s_Age;
            if (s_Age > m_Stats.max_latency_ms)
                m_Stats.max_latency_ms = s_Age;

            s_Result.status = Status::Applied;
            s_Result.observed.emplace_back("scene_available", Bool(p_Frame.scene_available));
            s_Result.observed.emplace_back("mission_playing", Bool(p_Frame.mission_playing));
            s_Result.observed.emplace_back("frame", Integer(static_cast<int64_t>(p_Frame.frame)));
            s_Result.observed.emplace_back("receipt_to_frame_ms", Integer(static_cast<int64_t>(s_Age)));

            if (p_Frame.scene_available)
            {
                s_Result.observed.emplace_back("scene_loaded", Bool(p_Frame.scene.scene_loaded));
                s_Result.observed.emplace_back("loading_stage", Integer(p_Frame.scene.loading_stage));
                s_Result.observed.emplace_back("scene_type", Text(p_Frame.scene.scene_type));
                s_Result.observed.emplace_back("codename_hint", Text(p_Frame.scene.codename_hint));
            }
            else
            {
                // The observation could not be obtained this frame: said so, never guessed.
                s_Result.reason = "scene state unavailable this frame";
            }

            m_Queue.MarkDone(s_Command.command_id, s_Pending->link_generation, Status::Applied);
            m_Emit(s_Result);
            return true;
        }

        case CommandProtocol::Entry::PlayerOutfitSet1Reserved:
        case CommandProtocol::Entry::Unknown:
        default:
            // The link never queues these; a defensive rejection keeps the invariant visible.
            ++m_Stats.rejected;
            s_Result.status = Status::Rejected;
            s_Result.reason = "not executable";
            m_Queue.MarkDone(s_Command.command_id, s_Pending->link_generation, Status::Rejected);
            m_Emit(s_Result);
            return true;
    }
}
