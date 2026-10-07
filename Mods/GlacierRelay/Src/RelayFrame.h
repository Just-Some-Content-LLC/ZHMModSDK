#pragma once

#include <functional>
#include <optional>
#include <string>

#include "MissionObserver.h"
#include "RelayAdapter.h"
#include "SceneState.h"
#include "TelemetryNormalizer.h"
#include "TelemetryQueue.h"

// The per-frame sequence of the relay, engine-independent so its order can be tested.
//
// Intentional semantics (M2 B1): telemetry captured while the previous mission-playing state was
// authoritative is drained and published BEFORE the next observed lifecycle edge is processed.
// A supported telemetry occurrence recorded late in a mission therefore publishes while the
// attempt is still open, and mission.stopped follows it:
//
//     ... actor.died / actor.pacified #N, mission.stopped #(N+1)
//
// Reversing the two steps would turn terminal telemetry into unattributed observations.
namespace RelayFrame
{
    struct Result
    {
        size_t outcomes_published = 0;
        size_t outside_attempt = 0; // normalized, but no attempt was open: not published
        size_t malformed = 0;
        bool playing_before = false;
        bool playing_after = false;
        bool edge_published = false;
    };

    // p_Scene is this frame's scene observation; nullopt when the engine state could not be read,
    // in which case step 2 is skipped (the observer keeps its state, as it always has) and only the
    // queued telemetry is processed. p_Warn receives one line per malformed or outside-attempt
    // observation (for the durable log).
    Result Process(
        TelemetryQueue& p_Queue, TelemetryNormalizer& p_Normalizer, MissionObserver& p_Observer, RelayAdapter* p_Adapter,
        const std::optional<SceneState>& p_Scene, const std::optional<std::string>& p_GameSessionId,
        const std::function<void(const std::string&)>& p_Warn
    );
}
