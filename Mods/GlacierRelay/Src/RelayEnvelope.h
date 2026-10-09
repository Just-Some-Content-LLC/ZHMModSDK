#pragma once

#include <cstdint>
#include <string>

#include "RelayEvent.h"

// The envelope every published message carries (glacier-relay M1 design, section 4, and
// protocol/README.md). Serialized as one JSON object on one line; the wire (stage 2) adds the
// newline. All fields are adapter-owned values.
namespace RelayProtocol
{
    constexpr int k_ProtocolVersion = 1;
}

struct RelayEnvelope
{
    int protocol_version = RelayProtocol::k_ProtocolVersion;
    std::string adapter_instance_id; // one per adapter instance (game process)
    uint64_t sequence = 0;           // 1, 2, 3, ... per instance; gaps mean drops
    std::string timestamp;           // ISO 8601 UTC with milliseconds, e.g. 2026-10-06T20:34:34.787Z
    std::string event_type;          // e.g. mission.playing
    int schema_version = 0;          // of the payload for this event_type
    std::string payload_json;        // already-serialized JSON object
};

namespace RelaySerialization
{
    // Schema version 1 of both mission lifecycle payloads: the same four fields.
    std::string MissionScenePayloadJson(const MissionScenePayload& p_Payload);
    // Schema version 1 of actor.died / actor.pacified: the same shape for both.
    std::string ActorOutcomePayloadJson(const ActorOutcomeEvent& p_Event);
    // Schema version 1 of contract.started and contract.ended.
    std::string ContractStartedPayloadJson(const ContractStartedEvent& p_Event);
    std::string ContractEndedPayloadJson(const ContractEndedEvent& p_Event);
    // Schema version 1 of disguise.equipped / disguise.compromised / disguise.compromise_cleared:
    // the same shape, plus "kind" on disguise.equipped only.
    std::string DisguisePayloadJson(const DisguiseEvent& p_Event);
    // Schema version 1 of item.picked_up / item.thrown / item.removed_from_inventory: one shape;
    // optional fields are absent, never empty or null.
    std::string ItemPayloadJson(const ItemEvent& p_Event);
    std::string Envelope(const RelayEnvelope& p_Envelope);
}
