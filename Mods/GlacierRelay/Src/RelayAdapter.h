#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "IRelaySink.h"
#include "RelayEvent.h"

// Turns semantic events into envelopes and hands them to the sink. Owns the adapter instance id
// and the sequence counter, which is one counter across every event type. The clock is injected
// so serialization can be tested exactly.
class RelayAdapter
{
public:
    using Clock = std::function<std::string()>;

    RelayAdapter(std::unique_ptr<IRelaySink> p_Sink, std::string p_InstanceId, Clock p_Clock);

    // A random UUID (version 4) as text.
    static std::string NewInstanceId();

    // Current UTC time as ISO 8601 with milliseconds.
    static std::string UtcNow();

    // One overload per event type; each knows its own type name and schema version.
    void Publish(const MissionPlayingEvent& p_Event);
    void Publish(const MissionStoppedEvent& p_Event);
    void Publish(const MissionEvent& p_Event);
    void Publish(const ActorOutcomeEvent& p_Event);

    const std::string& InstanceId() const { return m_InstanceId; }
    uint64_t PublishedCount() const { return m_Sequence; }

private:
    void PublishEnvelope(const char* p_EventType, int p_SchemaVersion, std::string p_PayloadJson);

private:
    std::unique_ptr<IRelaySink> m_Sink;
    std::string m_InstanceId;
    Clock m_Clock;
    uint64_t m_Sequence = 0;
};
