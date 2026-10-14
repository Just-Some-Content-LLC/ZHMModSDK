#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "OutfitObservation.h"

// The one engine-facing surface of the effectful outfit path (M5 C-2B; M5 design section 25 §4).
// OutfitApply (engine-independent) drives an implementation of this interface on the applying
// frame: Observe resolves the hero and the candidate kit fresh and reports what it read; Invoke
// makes the single approved engine call with the references Observe resolved on this same frame;
// ReadBack reads the hero's tuple and the registry again after the call; Release drops every
// reference before the frame ends. The DLL's implementation (OutfitApplier.cpp, Glacier-facing) is
// the only place in the adapter that calls Functions::ZHitman5_SetOutfit; the tests and the
// command probe implement fakes over scripted state and never construct an engine pointer.
namespace OutfitApply
{
    enum class Invocation
    {
        NotCalled, // the gate refused, or the link or thread check failed: nothing was asked of the engine
        Called,    // the setter was invoked once and returned (a void return proves nothing about the state)
        Faulted,   // the setter raised a structured exception (see OutfitApplier.cpp for what that means in the DLL)
    };

    enum class Effect
    {
        None,      // nothing was invoked
        Changed,   // the post-call tuple differs from the prior tuple and equals the requested one
        Unchanged, // the post-call tuple equals the prior tuple
        Uncertain, // fault, missing or inconsistent read-back, a generation change, or any other indeterminate outcome
    };

    const char* InvocationText(Invocation p_Invocation);
    const char* EffectText(Effect p_Effect);

    struct CallOutcome
    {
        bool called = false;  // the call was made
        bool faulted = false; // a structured exception was raised inside it
    };

    class IOutfitApplier
    {
    public:
        virtual ~IOutfitApplier() = default;

        // The frame-local reads (as relay.inspect.outfit makes them), resolving and holding the
        // hero and candidate references for Invoke on this frame.
        virtual OutfitObservation::EngineReads Observe(const std::optional<std::string>& p_CandidateId) = 0;

        // Exactly one call of the approved setter with the references of the last Observe.
        virtual CallOutcome Invoke(int32_t p_Charset, int32_t p_Variation) = 0;

        // Fresh reads after the call: the hero's tuple and the registry generation.
        virtual OutfitObservation::EngineReads ReadBack() = 0;

        // True once a fault has latched the effectful path off for the rest of the process.
        virtual bool Latched() const = 0;

        // Drops every reference held since Observe.
        virtual void Release() = 0;
    };
}
