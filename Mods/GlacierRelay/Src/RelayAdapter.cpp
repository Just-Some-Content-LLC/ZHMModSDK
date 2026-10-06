#include "RelayAdapter.h"

#include <Windows.h>

#include <random>

#include <fmt/format.h>

#include "RelayEnvelope.h"
#include "RelayLog.h"

RelayAdapter::RelayAdapter(std::unique_ptr<IRelaySink> p_Sink, std::string p_InstanceId, Clock p_Clock) :
    m_Sink(std::move(p_Sink)),
    m_InstanceId(std::move(p_InstanceId)),
    m_Clock(std::move(p_Clock))
{
}

std::string RelayAdapter::NewInstanceId()
{
    std::random_device s_Device;
    std::uniform_int_distribution<uint32_t> s_Word;

    uint32_t s_Words[4] = {s_Word(s_Device), s_Word(s_Device), s_Word(s_Device), s_Word(s_Device)};
    s_Words[1] = (s_Words[1] & 0xFFFF0FFFu) | 0x00004000u; // version 4
    s_Words[2] = (s_Words[2] & 0x3FFFFFFFu) | 0x80000000u; // RFC 4122 variant

    return fmt::format(
        "{:08x}-{:04x}-{:04x}-{:04x}-{:04x}{:08x}",
        s_Words[0], s_Words[1] >> 16, s_Words[1] & 0xFFFFu, s_Words[2] >> 16, s_Words[2] & 0xFFFFu, s_Words[3]
    );
}

std::string RelayAdapter::UtcNow()
{
    SYSTEMTIME s_Now;
    GetSystemTime(&s_Now);

    return fmt::format(
        "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
        s_Now.wYear, s_Now.wMonth, s_Now.wDay, s_Now.wHour, s_Now.wMinute, s_Now.wSecond, s_Now.wMilliseconds
    );
}

void RelayAdapter::Publish(const MissionPlayingEvent& p_Event)
{
    RelayEnvelope s_Envelope;
    s_Envelope.adapter_instance_id = m_InstanceId;
    s_Envelope.sequence = ++m_Sequence;
    s_Envelope.timestamp = m_Clock();
    s_Envelope.event_type = RelayEvents::k_MissionPlaying;
    s_Envelope.schema_version = RelayEvents::k_MissionPlayingSchemaVersion;
    s_Envelope.payload_json = RelaySerialization::MissionPlayingPayload(p_Event);

    PublishedEnvelope s_Published;
    s_Published.event_type = s_Envelope.event_type;
    s_Published.sequence = s_Envelope.sequence;
    s_Published.json = RelaySerialization::Envelope(s_Envelope);

    // The complete envelope, logged once at the publication boundary, whatever the sink. This is
    // the native side of the field-by-field comparison with what BEAM decodes (M1 final run, open
    // item 1); sinks log only their own delivery outcome.
    RelayLog::Info("published {} #{}: {}", s_Published.event_type, s_Published.sequence, s_Published.json);

    m_Sink->Publish(s_Published);
}
