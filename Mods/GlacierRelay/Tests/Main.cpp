#include "TestHarness.h"

void RunMissionObserverTests();
void RunRelayAdapterTests();
void RunTcpRelaySinkTests();
void RunTelemetryNormalizerTests();
void RunTelemetryQueueTests();
void RunRelayFrameTests();
void RunContractLifecycleTests();
void RunDisguiseTelemetryTests();
void RunRepositoryIdTests();
void RunItemTelemetryTests();
void RunObjectiveTelemetryTests();
void RunPlayerStateTelemetryTests();
void RunCommandProtocolTests();
void RunCommandQueueTests();
void RunCommandSchedulerTests();
void RunCommandLinkTests();
void RunOutfitInspectionTests();
void RunAttestationTests();
void RunOutfitApplyTests();
void RunPlayerInspectionTests();
void RunInspectionBoundaryTests();
void RunCandidateInspectionTests();
void RunPropertyGuardTests();
void RunTransformObservationTests();
void RunTransformApplyTests();
void RunMotionProtocolTests();
void RunMotionLinkTests();
void RunMotionSessionTests();
void RunMotionRestoreTests();
void RunMotionShareTests();
void RunMotionVerifierTests();

int main()
{
    RunMissionObserverTests();
    RunRelayAdapterTests();
    RunTelemetryNormalizerTests();
    RunTelemetryQueueTests();
    RunRelayFrameTests();
    RunContractLifecycleTests();
    RunDisguiseTelemetryTests();
    RunRepositoryIdTests();
    RunItemTelemetryTests();
    RunObjectiveTelemetryTests();
    RunPlayerStateTelemetryTests();
    RunTcpRelaySinkTests();
    RunCommandProtocolTests();
    RunCommandQueueTests();
    RunCommandSchedulerTests();
    RunCommandLinkTests();
    RunOutfitInspectionTests();
    RunAttestationTests();
    RunOutfitApplyTests();
    RunPlayerInspectionTests();
    RunInspectionBoundaryTests();
    RunCandidateInspectionTests();
    RunPropertyGuardTests();
    RunTransformObservationTests();
    RunTransformApplyTests();
    RunMotionProtocolTests();
    RunMotionLinkTests();
    RunMotionSessionTests();
    RunMotionRestoreTests();
    RunMotionShareTests();
    RunMotionVerifierTests();

    if (g_Failures == 0)
        std::printf("GlacierRelayTests: all checks passed\n");
    else
        std::printf("GlacierRelayTests: %d check(s) failed\n", g_Failures);

    return g_Failures == 0 ? 0 : 1;
}
