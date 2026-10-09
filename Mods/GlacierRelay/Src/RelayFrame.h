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
//
// M2 B2: the normalizer's table says per source event whether publication is gated on the
// predicate. Attempt-gated events (actor outcomes) follow the rule above. Ungated events (contract
// lifecycle) publish whenever captured and valid, in the same drain and the same sequence, because
// Glacier emits them before the rise and after the fall (design section 27.3); the plugin reports
// the occurrence and BEAM correlates it. No grace window, no attachment here.
//
// M2 B3: disguise events are attempt-gated. What the drain-before-edge order does and does not
// guarantee (design section 30.7): an observation already queued when this frame drains is judged
// against the pre-edge state and publishes before this frame's edge; an occurrence the engine
// emits later in the same frame is drained on the next processed frame, after the edge, and for an
// attempt-gated row that is "outside attempt": counted and logged, never published.
//
// M2 B4: item events are attempt-gated on the same evidence and follow the same rules; each
// occurrence the engine emits is one publication (a removal and a throw drained in one frame are
// two consecutive sequences, as captured).
namespace RelayFrame
{
    struct Result
    {
        size_t outcomes_published = 0; // attempt-gated events published (actor outcomes, disguise, items)
        size_t ungated_published = 0;  // ungated events published (contract lifecycle)
        size_t outside_attempt = 0;    // attempt-gated and normalized, but no attempt was open: not published
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
