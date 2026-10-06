#pragma once

#include <cstdint>
#include <string>

// What a sink receives: the serialized envelope plus two fields copied out of it for routing and
// logging. Owned values only. By construction nothing from the engine reaches this point except as
// text inside the JSON (glacier-relay ADR 0005).
struct PublishedEnvelope
{
    std::string event_type;
    uint64_t sequence = 0;
    std::string json; // one JSON object, no trailing newline
};

// Glacier Relay's native boundary. Implementations deliver envelopes somewhere; the adapter does
// not care where. M1 stage 1 ships LogRelaySink only. This is not IHitmenTransport and must not
// grow toward it: no listen, no connection handles, no inbound bytes.
class IRelaySink
{
public:
    virtual ~IRelaySink() = default;

    // Called on the frame thread. Must not block.
    virtual void Publish(const PublishedEnvelope& p_Envelope) = 0;
};
