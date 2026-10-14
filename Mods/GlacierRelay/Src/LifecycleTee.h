#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "IRelaySink.h"

// A sink decorator (M5 C-1): forwards every envelope unchanged to the real sink and, for the two
// contract lifecycle envelopes only (contract.started, contract.ended), reports the contract
// session id they carry to a frame-thread callback. The command path's transition instrumentation
// learns about contract ends this way without touching the telemetry intake, the normalizer, the
// frame sequencer or the adapter: it reads the published JSON text after the fact, strictly and
// bounded, on the frame thread that publishes it. Installed only when the command link is enabled;
// with it disabled the sink chain is exactly the M2 one. Nothing here touches the engine.
class LifecycleTee final : public IRelaySink
{
public:
    enum class Kind { ContractStarted, ContractEnded };

    using Observer = std::function<void(Kind, const std::string& p_ContractSessionId)>;

    LifecycleTee(std::unique_ptr<IRelaySink> p_Inner, Observer p_Observer);

    void Publish(const PublishedEnvelope& p_Envelope) override;

    struct Stats
    {
        uint64_t forwarded = 0;
        uint64_t observed = 0;   // lifecycle envelopes whose session id was read
        uint64_t unreadable = 0; // lifecycle envelopes whose payload could not be parsed or had no id
    };

    Stats GetStats() const { return m_Stats; }

    // Exposed for tests: the session id of a lifecycle envelope's JSON, if readable.
    static std::optional<std::string> ContractSessionId(const std::string& p_Json);

private:
    std::unique_ptr<IRelaySink> m_Inner;
    Observer m_Observer;
    Stats m_Stats;
};
