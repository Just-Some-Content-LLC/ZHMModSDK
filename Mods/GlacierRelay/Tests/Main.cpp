#include "TestHarness.h"

void RunMissionObserverTests();
void RunRelayAdapterTests();
void RunTcpRelaySinkTests();
void RunTelemetryNormalizerTests();
void RunTelemetryQueueTests();

int main()
{
    RunMissionObserverTests();
    RunRelayAdapterTests();
    RunTelemetryNormalizerTests();
    RunTelemetryQueueTests();
    RunTcpRelaySinkTests();

    if (g_Failures == 0)
        std::printf("GlacierRelayTests: all checks passed\n");
    else
        std::printf("GlacierRelayTests: %d check(s) failed\n", g_Failures);

    return g_Failures == 0 ? 0 : 1;
}
