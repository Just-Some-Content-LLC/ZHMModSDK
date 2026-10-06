#include "RelayEnvelope.h"

#include <fmt/format.h>

#include "Json.h"

std::string RelaySerialization::MissionPlayingPayload(const MissionPlayingEvent& p_Event)
{
    std::string s_Json = fmt::format(
        "{{\"scene_resource\":{},\"scene_type\":{},\"codename_hint\":{}",
        Json::Quote(p_Event.scene_resource), Json::Quote(p_Event.scene_type), Json::Quote(p_Event.codename_hint)
    );

    if (p_Event.game_session_id)
        s_Json += fmt::format(",\"game_session_id\":{}", Json::Quote(*p_Event.game_session_id));

    s_Json += "}";
    return s_Json;
}

std::string RelaySerialization::Envelope(const RelayEnvelope& p_Envelope)
{
    return fmt::format(
        "{{\"protocol_version\":{},\"adapter_instance_id\":{},\"sequence\":{},\"timestamp\":{},"
        "\"event_type\":{},\"schema_version\":{},\"payload\":{}}}",
        p_Envelope.protocol_version, Json::Quote(p_Envelope.adapter_instance_id), p_Envelope.sequence,
        Json::Quote(p_Envelope.timestamp), Json::Quote(p_Envelope.event_type), p_Envelope.schema_version,
        p_Envelope.payload_json
    );
}
