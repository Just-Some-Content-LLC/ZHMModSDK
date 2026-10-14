#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "CommandProtocol.h"
#include "CommandQueue.h"
#include "IOutfitApplier.h"
#include "PreconditionEvaluator.h"
#include "SceneState.h"

// The effectful apply of player.outfit.set v2 (M5 C-2B; glacier-relay
// docs/design/M5C2_EFFECTFUL_OUTFIT_DESIGN.md sections 3–4 and 9). Engine-independent: it drives
// an IOutfitApplier on the applying frame and never touches the engine itself. In order, on one
// frame, inside the scheduler's dispatch: (1) the gate — PreconditionEvaluator over the fresh reads
// of this frame with the effectful rules, every failure reported with its reasons and nothing
// asked of the engine; (2) the prior tuple recorded from those reads; (3) the link generation
// re-read immediately before the call; (4) the "applying" line written to the durable native log
// before the call; (5) exactly one Invoke; (6) the read-back after the call; (7) the effect
// classification — changed / unchanged / uncertain — and the bounded result. A void setter that
// returned is never, by itself, a changed state; a fault is never "definitely unapplied".
namespace OutfitApply
{
    // What the scheduler copies from its frame observation for one apply (plain values only).
    struct Context
    {
        const CommandQueue::Pending* pending = nullptr;
        uint64_t frame = 0;
        uint64_t now_mono_ms = 0;
        uint32_t thread_id = 0;
        uint32_t expected_thread_id = 0; // the scheduler's first frame thread (0: unknown yet, accepted)
        bool scene_available = false;
        SceneState scene;
        bool mission_playing = false;
        bool previous_frame_playing = false;
        std::optional<std::string> mission_generation; // edge-cached
        uint64_t link_generation = 0;                  // the frame's copy
        std::function<uint64_t()> link_generation_now; // re-read immediately before the call (null: the frame's copy)
        uint64_t relay_transition_epoch = 0;
        uint64_t rise_frame = 0;
        uint64_t rise_mono_ms = 0;
        bool scene_resource_same_as_rise = false;
        std::optional<std::string> contract_started_seen;
        std::optional<std::string> contract_ended_seen;
        PreconditionEvaluator::Rules rules;
    };

    struct Outcome
    {
        Invocation invocation = Invocation::NotCalled;
        Effect effect = Effect::None;
        bool precondition_ok = false;
        bool reduced = false; // the result had to be reduced to fit the line bound
    };

    // Fills p_Result (status, reason, observed) and returns the outcome. The applier is driven
    // only when the gate holds; it is always Released before returning.
    Outcome Apply(const Context& p_Context, IOutfitApplier& p_Applier, CommandProtocol::Result& p_Result);
}
