#pragma once

#include "IRelaySink.h"

// Stage 1 sink: writes each envelope to the durable log and nowhere else.
class LogRelaySink : public IRelaySink
{
public:
    void Publish(const PublishedEnvelope& p_Envelope) override;
};
