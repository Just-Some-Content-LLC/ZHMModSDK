#include "TransformApplier.h"

#include <Windows.h>

#include "Glacier/ZMath.h"
#include "Glacier/ZSpatialEntity.h"

#include "RelayLog.h"

using OutfitObservation::Field;
using OutfitObservation::FieldStatus;

namespace
{
    TransformObservation::Reads FaultedReads(const char* p_Why)
    {
        auto s_Reads = TransformObservation::AllUnavailable(FieldStatus::Unavailable, p_Why);
        s_Reads.thread_id = GetCurrentThreadId();
        return s_Reads;
    }
}

TransformObservation::Reads EngineTransformApplier::Observe()
{
    TransformObservation::Reads s_Reads;
    bool s_Completed = false;

    RelayLog::Guard("TransformApplier::Observe", [&] {
        s_Reads = SceneObservation::ObserveCandidateTransform(&m_Handles);
        s_Completed = true;
    });

    if (!s_Completed)
    {
        m_Latched = true;
        m_Handles = {};
        return FaultedReads("fault during the apply-frame reads");
    }

    return s_Reads;
}

// The one call site. The spatial interface was resolved by Observe on this same frame; nothing is
// cached across frames. The by-value SMatrix is reconstructed through the SDK's declared
// conversion from the twelve words (SMatrix43: three axes and the translation; the conversion adds
// the homogeneous components, 0 for the axes and 1 for the translation); a 48-byte SMatrix43 is
// never reinterpreted as the 64-byte argument.
TransformApply::CallOutcome EngineTransformApplier::Invoke(const FloatWords::Words12& p_World)
{
    TransformApply::CallOutcome s_Outcome;

    if (m_Latched || RelayLog::FaultsLogged() > 0)
        return s_Outcome;

    if (!m_Handles.candidate || !m_Handles.spatial)
        return s_Outcome;

    if (!FloatWords::AllFinite(p_World))
        return s_Outcome;

    SMatrix43 s_Stored;
    s_Stored.XAxis = SVector3(FloatWords::ToFloat(p_World.w[0]), FloatWords::ToFloat(p_World.w[1]), FloatWords::ToFloat(p_World.w[2]));
    s_Stored.YAxis = SVector3(FloatWords::ToFloat(p_World.w[3]), FloatWords::ToFloat(p_World.w[4]), FloatWords::ToFloat(p_World.w[5]));
    s_Stored.ZAxis = SVector3(FloatWords::ToFloat(p_World.w[6]), FloatWords::ToFloat(p_World.w[7]), FloatWords::ToFloat(p_World.w[8]));
    s_Stored.Trans = SVector3(FloatWords::ToFloat(p_World.w[9]), FloatWords::ToFloat(p_World.w[10]), FloatWords::ToFloat(p_World.w[11]));
    const SMatrix s_World(s_Stored);

    ZSpatialEntity* s_Spatial = m_Handles.spatial;
    bool s_Returned = false;

    RelayLog::Guard("ApplyTransform (ZSpatialEntity::SetObjectToWorldMatrixFromEditor)", [&] {
        s_Spatial->SetObjectToWorldMatrixFromEditor(s_World);
        s_Returned = true;
    });

    s_Outcome.called = true;

    if (!s_Returned)
    {
        // Reachable only if an outer handler resumed the process after the fault (see the header).
        s_Outcome.faulted = true;
        m_Latched = true;
        RelayLog::Error("ApplyTransform: the setter faulted and the process continued; the effectful path is latched off for the rest of this process (fail-stop); the effect is uncertain");
    }

    return s_Outcome;
}

TransformObservation::Reads EngineTransformApplier::ReadBack()
{
    TransformObservation::Reads s_Reads;
    bool s_Completed = false;

    RelayLog::Guard("TransformApplier::ReadBack", [&] {
        s_Reads = SceneObservation::ObserveCandidateTransform(&m_Handles);
        s_Completed = true;
    });

    if (!s_Completed)
    {
        m_Latched = true;
        return FaultedReads("fault during the read-back after the call");
    }

    return s_Reads;
}

bool EngineTransformApplier::Latched() const
{
    return m_Latched || RelayLog::FaultsLogged() > 0;
}

void EngineTransformApplier::Release()
{
    m_Handles = {};
}
