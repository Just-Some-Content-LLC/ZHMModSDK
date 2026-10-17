#pragma once

#include "ITransformApplier.h"
#include "SceneObservation.h"

// The DLL's transform applier (M6-F; Glacier-facing): the only place in the adapter that calls
// ZSpatialEntity::SetObjectToWorldMatrixFromEditor, and — beside the outfit applier's one
// ZHitman5_SetOutfit call — the only engine mutation the adapter can make. It is constructed by
// the plugin only when [command] effectful is true, and even then TransformApply drives it only
// after every rule of the gate held on the applying frame.
//
// Fault semantics are the outfit applier's (OutfitApplier.h): RelayLog::Guard is an SEH filter
// that logs and returns EXCEPTION_CONTINUE_SEARCH — it never handles the exception, so a fault
// raised inside the setter keeps propagating to the engine's own handling, which on this build
// terminates the process. That is the fail-stop: the "applying" line already accepted into the
// native log file and the FAULT line are the evidence that the engine was asked, and BEAM holds
// the command as unknown with a possible effect (link lost). Invoke can return "faulted" only if
// some outer handler resumed the process, in which case the effect is reported uncertain and the
// latch refuses every later effectful command; the adapter never swallows the exception itself.
class EngineTransformApplier final : public TransformApply::ITransformApplier
{
public:
    TransformObservation::Reads Observe() override;
    TransformApply::CallOutcome Invoke(const FloatWords::Words12& p_World) override;
    TransformObservation::Reads ReadBack() override;
    bool Latched() const override;
    void Release() override;

private:
    SceneObservation::TransformHandles m_Handles;
    bool m_Latched = false;
};
