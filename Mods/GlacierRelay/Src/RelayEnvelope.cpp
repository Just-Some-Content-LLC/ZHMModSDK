#include "RelayEnvelope.h"

#include <fmt/format.h>

#include "Json.h"

std::string RelaySerialization::MissionScenePayloadJson(const MissionScenePayload& p_Payload)
{
    std::string s_Json = fmt::format(
        "{{\"scene_resource\":{},\"scene_type\":{},\"codename_hint\":{}",
        Json::Quote(p_Payload.scene_resource), Json::Quote(p_Payload.scene_type), Json::Quote(p_Payload.codename_hint)
    );

    if (p_Payload.game_session_id)
        s_Json += fmt::format(",\"game_session_id\":{}", Json::Quote(*p_Payload.game_session_id));

    s_Json += "}";
    return s_Json;
}

std::string RelaySerialization::ActorOutcomePayloadJson(const ActorOutcomeEvent& p_Event)
{
    std::string s_Json = fmt::format(
        "{{\"source\":{},\"repository_id\":{},\"actor_name\":{},\"engine_actor_id\":{},\"actor_type\":{}",
        Json::Quote(RelayEvents::k_SourceEngineTelemetry), Json::Quote(p_Event.repository_id),
        Json::Quote(p_Event.actor_name), p_Event.engine_actor_id, Json::Quote(p_Event.actor_type)
    );

    if (p_Event.actor_type_code)
        s_Json += fmt::format(",\"actor_type_code\":{}", *p_Event.actor_type_code);

    s_Json += fmt::format(
        ",\"is_target\":{},\"death_type\":{}", p_Event.is_target ? "true" : "false", Json::Quote(p_Event.death_type)
    );

    if (p_Event.death_type_code)
        s_Json += fmt::format(",\"death_type_code\":{}", *p_Event.death_type_code);

    s_Json += fmt::format(",\"death_context\":{}", Json::Quote(p_Event.death_context));

    if (p_Event.death_context_code)
        s_Json += fmt::format(",\"death_context_code\":{}", *p_Event.death_context_code);

    s_Json += fmt::format(
        ",\"accident\":{},\"kill_class\":{},\"method_broad\":{},\"method_strict\":{},\"damage_events\":[",
        p_Event.accident ? "true" : "false", Json::Quote(p_Event.kill_class), Json::Quote(p_Event.method_broad),
        Json::Quote(p_Event.method_strict)
    );

    for (size_t i = 0; i < p_Event.damage_events.size(); ++i)
        s_Json += (i ? "," : "") + Json::Quote(p_Event.damage_events[i]);

    s_Json += "]";

    if (p_Event.item_repository_id)
        s_Json += fmt::format(",\"item_repository_id\":{}", Json::Quote(*p_Event.item_repository_id));

    if (p_Event.contract_session_id)
        s_Json += fmt::format(",\"contract_session_id\":{}", Json::Quote(*p_Event.contract_session_id));

    if (p_Event.engine_timestamp_s)
        s_Json += fmt::format(",\"engine_timestamp_s\":{}", *p_Event.engine_timestamp_s);

    s_Json += "}";
    return s_Json;
}

std::string RelaySerialization::ContractStartedPayloadJson(const ContractStartedEvent& p_Event)
{
    std::string s_Json = fmt::format(
        "{{\"source\":{},\"engine_event\":{},\"contract_session_id\":{},\"contract_id\":{},\"location_id\":{},"
        "\"contract_type\":{},\"difficulty_level\":{},\"starting_disguise_repository_id\":{},\"is_hitman_suit\":{}",
        Json::Quote(RelayEvents::k_SourceEngineTelemetry), Json::Quote(p_Event.engine_event),
        Json::Quote(p_Event.contract_session_id), Json::Quote(p_Event.contract_id), Json::Quote(p_Event.location_id),
        Json::Quote(p_Event.contract_type), p_Event.difficulty_level, Json::Quote(p_Event.starting_disguise_repository_id),
        p_Event.is_hitman_suit ? "true" : "false"
    );

    if (p_Event.engine_timestamp_s)
        s_Json += fmt::format(",\"engine_timestamp_s\":{}", *p_Event.engine_timestamp_s);

    s_Json += "}";
    return s_Json;
}

std::string RelaySerialization::ContractEndedPayloadJson(const ContractEndedEvent& p_Event)
{
    std::string s_Json = fmt::format(
        "{{\"source\":{},\"engine_event\":{},\"contract_session_id\":{},\"contract_id\":{},\"reason\":{},"
        "\"reason_kind\":{}",
        Json::Quote(RelayEvents::k_SourceEngineTelemetry), Json::Quote(p_Event.engine_event),
        Json::Quote(p_Event.contract_session_id), Json::Quote(p_Event.contract_id), Json::Quote(p_Event.reason),
        Json::Quote(p_Event.reason_kind)
    );

    if (p_Event.engine_timestamp_s)
        s_Json += fmt::format(",\"engine_timestamp_s\":{}", *p_Event.engine_timestamp_s);

    s_Json += "}";
    return s_Json;
}

std::string RelaySerialization::DisguisePayloadJson(const DisguiseEvent& p_Event)
{
    std::string s_Json = fmt::format("{{\"source\":{}", Json::Quote(RelayEvents::k_SourceEngineTelemetry));

    // The Relay-owned distinction between the initial assertion and a change travels only on
    // disguise.equipped; the other two types have no kind.
    if (p_Event.kind == DisguiseEvent::Kind::Initial)
        s_Json += ",\"kind\":\"initial\"";
    else if (p_Event.kind == DisguiseEvent::Kind::Change)
        s_Json += ",\"kind\":\"change\"";

    s_Json += fmt::format(
        ",\"engine_event\":{},\"disguise_repository_id\":{}", Json::Quote(p_Event.engine_event),
        Json::Quote(p_Event.disguise_repository_id)
    );

    if (p_Event.contract_session_id)
        s_Json += fmt::format(",\"contract_session_id\":{}", Json::Quote(*p_Event.contract_session_id));

    if (p_Event.engine_timestamp_s)
        s_Json += fmt::format(",\"engine_timestamp_s\":{}", *p_Event.engine_timestamp_s);

    s_Json += "}";
    return s_Json;
}

std::string RelaySerialization::ItemPayloadJson(const ItemEvent& p_Event)
{
    std::string s_Json = fmt::format(
        "{{\"source\":{},\"engine_event\":{},\"item_repository_id\":{}", Json::Quote(RelayEvents::k_SourceEngineTelemetry),
        Json::Quote(p_Event.engine_event), Json::Quote(p_Event.item_repository_id)
    );

    if (p_Event.item_instance_id)
        s_Json += fmt::format(",\"item_instance_id\":{}", Json::Quote(*p_Event.item_instance_id));

    if (p_Event.item_name)
        s_Json += fmt::format(",\"item_name\":{}", Json::Quote(*p_Event.item_name));

    if (p_Event.item_type)
        s_Json += fmt::format(",\"item_type\":{}", Json::Quote(*p_Event.item_type));

    if (p_Event.online_traits)
    {
        s_Json += ",\"online_traits\":[";

        for (size_t i = 0; i < p_Event.online_traits->size(); ++i)
            s_Json += (i ? "," : "") + Json::Quote((*p_Event.online_traits)[i]);

        s_Json += "]";
    }

    if (p_Event.contract_session_id)
        s_Json += fmt::format(",\"contract_session_id\":{}", Json::Quote(*p_Event.contract_session_id));

    if (p_Event.engine_timestamp_s)
        s_Json += fmt::format(",\"engine_timestamp_s\":{}", *p_Event.engine_timestamp_s);

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
