#include "RelayFrame.h"

#include <fmt/format.h>

RelayFrame::Result RelayFrame::Process(
    TelemetryQueue& p_Queue, TelemetryNormalizer& p_Normalizer, MissionObserver& p_Observer, RelayAdapter* p_Adapter,
    const std::optional<SceneState>& p_Scene, const std::optional<std::string>& p_GameSessionId,
    const std::function<void(const std::string&)>& p_Warn
)
{
    Result s_Result;
    s_Result.playing_before = p_Observer.Playing();

    // Step 1: telemetry queued since the last frame, judged against the attempt state that was
    // authoritative when it was captured (the observer has not been updated yet this frame).
    for (const auto& s_Observation : p_Queue.Drain())
    {
        const auto s_Normalized = p_Normalizer.Normalize(s_Observation);

        switch (s_Normalized.outcome)
        {
            case TelemetryNormalizer::Outcome::Normalized:
                break;
            case TelemetryNormalizer::Outcome::Malformed:
                ++s_Result.malformed;
                p_Warn(fmt::format(
                    "telemetry '{}' (index {}) not normalized: {}", s_Observation.name, s_Observation.event_index,
                    s_Normalized.detail
                ));
                continue;
            case TelemetryNormalizer::Outcome::Unsupported:
            case TelemetryNormalizer::Outcome::DontSend:
                // Filtered at the intake before queueing; reaching here only means the intake and
                // the normalizer table disagree, and the normalizer's counters already record it.
                continue;
        }

        if (s_Normalized.gating == TelemetryNormalizer::Gating::AttemptGated && !p_Observer.Playing())
        {
            ++s_Result.outside_attempt;
            p_Warn(fmt::format(
                "telemetry '{}' (index {}) observed with no open mission attempt; not published",
                s_Observation.name, s_Observation.event_index
            ));
            continue;
        }

        if (!p_Adapter)
        {
            p_Warn(fmt::format("telemetry '{}' observed before the adapter existed; dropped", s_Observation.name));
            continue;
        }

        if (s_Normalized.event)
        {
            p_Adapter->Publish(*s_Normalized.event);
            ++s_Result.outcomes_published;
        }
        else if (s_Normalized.disguise)
        {
            // Attempt-gated like actor outcomes (M2 B3): the gate above already applied.
            p_Adapter->Publish(*s_Normalized.disguise);
            ++s_Result.outcomes_published;
        }
        else if (s_Normalized.item)
        {
            // Attempt-gated like actor outcomes and disguise (M2 B4): the gate above already applied.
            p_Adapter->Publish(*s_Normalized.item);
            ++s_Result.outcomes_published;
        }
        else if (s_Normalized.contract_started)
        {
            p_Adapter->Publish(*s_Normalized.contract_started);
            ++s_Result.ungated_published;
        }
        else if (s_Normalized.contract_ended)
        {
            p_Adapter->Publish(*s_Normalized.contract_ended);
            ++s_Result.ungated_published;
        }
        else if (s_Normalized.objective)
        {
            // Ungated (M2 B5): preserved with its payload wherever it drains relative to the edge;
            // the gate above did not apply. Attribution is BEAM's.
            p_Adapter->Publish(*s_Normalized.objective);
            ++s_Result.ungated_published;
        }
    }

    // Step 2: this frame's scene observation and, if the predicate moved, its edge.
    s_Result.playing_after = s_Result.playing_before;

    if (!p_Scene)
        return s_Result;

    const auto s_Edge = p_Observer.Update(*p_Scene, p_GameSessionId);
    s_Result.playing_after = p_Observer.Playing();

    if (s_Edge)
    {
        if (p_Adapter)
        {
            p_Adapter->Publish(*s_Edge);
            s_Result.edge_published = true;
        }
        else
        {
            p_Warn("mission lifecycle edge observed before the adapter existed; event dropped");
        }
    }

    return s_Result;
}
