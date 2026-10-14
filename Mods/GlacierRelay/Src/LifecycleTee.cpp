#include "LifecycleTee.h"

#include "CommandJson.h"
#include "RelayEvent.h"

LifecycleTee::LifecycleTee(std::unique_ptr<IRelaySink> p_Inner, Observer p_Observer) :
    m_Inner(std::move(p_Inner)),
    m_Observer(std::move(p_Observer))
{
}

std::optional<std::string> LifecycleTee::ContractSessionId(const std::string& p_Json)
{
    // The envelope is at most a few hundred bytes; the strict reader's bounds (depth 4, 256 nodes,
    // 1,024-byte strings) cover it, and a payload outside them is simply unreadable here.
    const auto s_Parsed = CommandJson::Parse(p_Json);

    if (!s_Parsed.ok || s_Parsed.value.kind != CommandJson::Value::Kind::Object)
        return std::nullopt;

    const auto* s_Payload = s_Parsed.value.Find("payload");
    if (!s_Payload || s_Payload->kind != CommandJson::Value::Kind::Object)
        return std::nullopt;

    const auto* s_Id = s_Payload->Find("contract_session_id");
    if (!s_Id || s_Id->kind != CommandJson::Value::Kind::String || s_Id->text.empty() || s_Id->text.size() > 64)
        return std::nullopt;

    return s_Id->text;
}

void LifecycleTee::Publish(const PublishedEnvelope& p_Envelope)
{
    const bool s_Started = p_Envelope.event_type == RelayEvents::k_ContractStarted;
    const bool s_Ended = p_Envelope.event_type == RelayEvents::k_ContractEnded;

    if (s_Started || s_Ended)
    {
        if (const auto s_Id = ContractSessionId(p_Envelope.json))
        {
            ++m_Stats.observed;
            if (m_Observer)
                m_Observer(s_Started ? Kind::ContractStarted : Kind::ContractEnded, *s_Id);
        }
        else
        {
            ++m_Stats.unreadable;
        }
    }

    ++m_Stats.forwarded;
    m_Inner->Publish(p_Envelope);
}
