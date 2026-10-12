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

    if (g_Failures == 0)
        std::printf("GlacierRelayTests: all checks passed\n");
    else
        std::printf("GlacierRelayTests: %d check(s) failed\n", g_Failures);

    return g_Failures == 0 ? 0 : 1;
}
