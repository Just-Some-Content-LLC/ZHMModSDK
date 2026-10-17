#pragma once

#include "IOutfitApplier.h"
#include "TransformObservation.h"

// The one engine-facing surface of the effectful transform path (M6-F; glacier-relay
// docs/design/M6D2_NEXT_STEP_DESIGN.md section 12.7). TransformApply (engine-independent) drives
// an implementation of this interface on the applying frame: Observe resolves the compiled
// candidate and the local hero fresh and reports their exact snapshots; Invoke makes the single
// approved engine call — ZSpatialEntity::SetObjectToWorldMatrixFromEditor with the world matrix
// reconstructed from the twelve words — on the spatial interface Observe resolved on this same
// frame; ReadBack re-resolves both entities and reads their snapshots again after the call, saying
// whether the candidate is the entity Observe resolved; Release drops every reference before the
// frame ends. The DLL's implementation (TransformApplier.cpp, Glacier-facing) is the only place in
// the adapter that calls the setter; the tests and the command probe implement a fake over
// scripted words and never construct an engine pointer.
namespace TransformApply
{
    using OutfitApply::CallOutcome;
    using OutfitApply::Effect;
    using OutfitApply::Invocation;

    class ITransformApplier
    {
    public:
        virtual ~ITransformApplier() = default;

        // The frame-local reads, resolving and holding the candidate's spatial interface and the
        // local hero for Invoke / ReadBack on this frame.
        virtual TransformObservation::Reads Observe() = 0;

        // Exactly one call of the approved setter with the world group given, on the candidate's
        // spatial interface of the last Observe.
        virtual CallOutcome Invoke(const FloatWords::Words12& p_World) = 0;

        // Fresh reads after the call (the candidate re-resolved; same_entity_as_before set).
        virtual TransformObservation::Reads ReadBack() = 0;

        // True once a fault has latched the effectful path off for the rest of the process.
        virtual bool Latched() const = 0;

        // Drops every reference held since Observe.
        virtual void Release() = 0;
    };
}
