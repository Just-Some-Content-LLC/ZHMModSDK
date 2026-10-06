#pragma once

#include "IRelaySink.h"

// The no-network sink: delivers each envelope to the durable log and nowhere else. Since M2 the
// envelope body itself is logged by RelayAdapter, so this sink adds only a delivery line.
class LogRelaySink : public IRelaySink
{
public:
    void Publish(const PublishedEnvelope& p_Envelope) override;
};
