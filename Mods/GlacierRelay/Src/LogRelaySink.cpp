#include "LogRelaySink.h"

#include "RelayLog.h"

void LogRelaySink::Publish(const PublishedEnvelope& p_Envelope)
{
    // The envelope body is logged by RelayAdapter at the publication boundary; this sink only
    // records that it was the delivery endpoint.
    RelayLog::Info("log sink: delivered {} #{}", p_Envelope.event_type, p_Envelope.sequence);
}
