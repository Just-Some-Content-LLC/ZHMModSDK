#include "RelayAdapter.h"

#include <Windows.h>

#include <random>
#include <variant>

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
    PublishEnvelope(
        RelayEvents::k_MissionPlaying, RelayEvents::k_MissionPlayingSchemaVersion,
        RelaySerialization::MissionScenePayloadJson(p_Event)
    );
}

void RelayAdapter::Publish(const MissionStoppedEvent& p_Event)
{
    PublishEnvelope(
        RelayEvents::k_MissionStopped, RelayEvents::k_MissionStoppedSchemaVersion,
        RelaySerialization::MissionScenePayloadJson(p_Event)
    );
}

void RelayAdapter::Publish(const ActorOutcomeEvent& p_Event)
{
    const bool s_Died = p_Event.kind == ActorOutcomeEvent::Kind::Died;
    PublishEnvelope(
        s_Died ? RelayEvents::k_ActorDied : RelayEvents::k_ActorPacified,
        s_Died ? RelayEvents::k_ActorDiedSchemaVersion : RelayEvents::k_ActorPacifiedSchemaVersion,
        RelaySerialization::ActorOutcomePayloadJson(p_Event)
    );
}

void RelayAdapter::Publish(const ContractStartedEvent& p_Event)
{
    PublishEnvelope(
        RelayEvents::k_ContractStarted, RelayEvents::k_ContractStartedSchemaVersion,
        RelaySerialization::ContractStartedPayloadJson(p_Event)
    );
}

void RelayAdapter::Publish(const ContractEndedEvent& p_Event)
{
    PublishEnvelope(
        RelayEvents::k_ContractEnded, RelayEvents::k_ContractEndedSchemaVersion,
        RelaySerialization::ContractEndedPayloadJson(p_Event)
    );
}

void RelayAdapter::Publish(const DisguiseEvent& p_Event)
{
    const char* s_Type = RelayEvents::k_DisguiseEquipped;
    int s_Version = RelayEvents::k_DisguiseEquippedSchemaVersion;

    switch (p_Event.kind)
    {
        case DisguiseEvent::Kind::Initial:
        case DisguiseEvent::Kind::Change:
            break;
        case DisguiseEvent::Kind::Compromised:
            s_Type = RelayEvents::k_DisguiseCompromised;
            s_Version = RelayEvents::k_DisguiseCompromisedSchemaVersion;
            break;
        case DisguiseEvent::Kind::CompromiseCleared:
            s_Type = RelayEvents::k_DisguiseCompromiseCleared;
            s_Version = RelayEvents::k_DisguiseCompromiseClearedSchemaVersion;
            break;
    }

    PublishEnvelope(s_Type, s_Version, RelaySerialization::DisguisePayloadJson(p_Event));
}

void RelayAdapter::Publish(const MissionEvent& p_Event)
{
    std::visit([this](const auto& p_Concrete) { Publish(p_Concrete); }, p_Event);
}

void RelayAdapter::PublishEnvelope(const char* p_EventType, int p_SchemaVersion, std::string p_PayloadJson)
{
    RelayEnvelope s_Envelope;
    s_Envelope.adapter_instance_id = m_InstanceId;
    s_Envelope.sequence = ++m_Sequence;
    s_Envelope.timestamp = m_Clock();
    s_Envelope.event_type = p_EventType;
    s_Envelope.schema_version = p_SchemaVersion;
    s_Envelope.payload_json = std::move(p_PayloadJson);

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
