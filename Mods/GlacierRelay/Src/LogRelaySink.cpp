#include "LogRelaySink.h"

#include "RelayLog.h"

void LogRelaySink::Publish(const PublishedEnvelope& p_Envelope)
{
    RelayLog::Info("published {} #{}: {}", p_Envelope.event_type, p_Envelope.sequence, p_Envelope.json);
}
