#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Logging.h"

// Transport seam for the Hitmen revival.
//
// Replaces the mod's direct use of GameNetworkingSockets 1.4.1. Each method maps to the GNS
// calls it stands in for, so the original transport contract (IP listen/connect, a single
// client, unreliable no-Nagle sends, polled receive) stays visible here.
//
// The dormant revival only ships NullHitmenTransport, which never opens a socket.

using HitmenConnection = uint32_t;
constexpr HitmenConnection k_InvalidHitmenConnection = 0;

struct HitmenMessage
{
    std::vector<uint8_t> m_Data;
};

class IHitmenTransport
{
public:
    virtual ~IHitmenTransport() = default;

    // Was: CreateListenSocketIP + CreatePollGroup. Accepted connections are reported by PollConnected.
    virtual bool StartServer(uint16_t p_Port) = 0;

    // Was: SteamNetworkingIPAddr::ParseString + ConnectByIPAddress.
    virtual bool Connect(const std::string& p_Address, uint16_t p_Port) = 0;

    // Was: RunCallbacks + the server/client connection status callbacks (accept, poll group assignment).
    // Returns a connection that became usable since the last call, or k_InvalidHitmenConnection.
    virtual HitmenConnection PollConnected() = 0;

    // Was: ReceiveMessagesOnPollGroup (server) / ReceiveMessagesOnConnection (client).
    virtual std::vector<HitmenMessage> ReceiveMessages() = 0;

    // Was: SendMessageToConnection(..., k_nSteamNetworkingSend_UnreliableNoNagle, nullptr).
    virtual void SendUnreliable(HitmenConnection p_Connection, const void* p_Data, size_t p_Size) = 0;
};

class NullHitmenTransport : public IHitmenTransport
{
public:
    bool StartServer(uint16_t p_Port) override
    {
        Logger::Warn("[Hitmen] Networking is disabled; not starting a server on port {}.", p_Port);
        return false;
    }

    bool Connect(const std::string& p_Address, uint16_t p_Port) override
    {
        Logger::Warn("[Hitmen] Networking is disabled; not connecting to {}:{}.", p_Address, p_Port);
        return false;
    }

    HitmenConnection PollConnected() override
    {
        return k_InvalidHitmenConnection;
    }

    std::vector<HitmenMessage> ReceiveMessages() override
    {
        return {};
    }

    void SendUnreliable(HitmenConnection p_Connection, const void* p_Data, size_t p_Size) override {}
};
