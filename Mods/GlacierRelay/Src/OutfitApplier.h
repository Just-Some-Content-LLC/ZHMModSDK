#pragma once

#include "IOutfitApplier.h"
#include "SceneObservation.h"

// The DLL's outfit applier (M5 C-2B; Glacier-facing): the only place in the adapter that calls
// Functions::ZHitman5_SetOutfit, and the only engine mutation the adapter can make. It is
// constructed by the plugin only when [command] effectful is true, and even then OutfitApply
// drives it only after every rule of the gate held on the applying frame.
//
// Fault semantics (M5 design section 25 §4.5, reviewed for C-2B): RelayLog::Guard is an SEH filter
// that logs and returns EXCEPTION_CONTINUE_SEARCH — it never handles the exception, so a fault
// raised inside the setter keeps propagating to the engine's own handling, which on this build
// terminates the process. That is the fail-stop: the "applying" line already in the durable
// native log (written by OutfitApply before Invoke) and the FAULT line are the evidence that the
// engine was asked, and BEAM holds the command as unknown with a possible effect (link lost).
// Invoke can therefore return "faulted" only if some outer handler resumed the process, in which
// case the effect is reported uncertain and the latch below refuses every later effectful
// command; the adapter never swallows the exception itself to keep running the engine call path.
class EngineOutfitApplier final : public OutfitApply::IOutfitApplier
{
public:
    OutfitObservation::EngineReads Observe(const std::optional<std::string>& p_CandidateId) override;
    OutfitApply::CallOutcome Invoke(int32_t p_Charset, int32_t p_Variation) override;
    OutfitObservation::EngineReads ReadBack() override;
    bool Latched() const override;
    void Release() override;

private:
    SceneObservation::OutfitHandles m_Handles;
    bool m_Latched = false;
};
