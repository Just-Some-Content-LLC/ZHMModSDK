#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "IPluginInterface.h"

#include "CommandLink.h"
#include "CommandQueue.h"
#include "CommandScheduler.h"
#include "LifecycleTee.h"
#include "MissionObserver.h"
#include "RelayAdapter.h"
#include "SceneState.h"
#include "TelemetryNormalizer.h"
#include "TelemetryQueue.h"

struct SGameUpdateEvent;
class ZAchievementManagerSimple;
class ZDynamicObject;

// Glacier Relay native adapter.
//
// Lifecycle: observe engine state each frame, derive the mission lifecycle edges (mission.playing,
// mission.stopped) and publish them through IRelaySink. No hook is involved.
//
// Telemetry (M2 B1, ADR 0006): one read-only detour on the engine's telemetry stream
// (ZAchievementManagerSimple::OnEventSent). The detour copies supported events into a bounded
// queue and continues; the frame update drains the queue through TelemetryNormalizer and
// publishes the resulting Relay events (actor.died, actor.pacified) while a mission attempt is
// open. No UI, no writes to engine state.
//
// Command link (M5-B, ADR 0009): optional and off by default ([command] enabled = false in
// glacierrelay.ini). When enabled, a second outbound loopback client (CommandLink, its own
// network thread, never a listener) receives allowlisted commands from BEAM; the frame update
// schedules at most one per frame through CommandScheduler. The only executable entry is
// relay.probe, which writes nothing and reports what the frame observed. The telemetry sink and
// the detour are untouched by it; when disabled none of it is constructed.
class GlacierRelay : public IPluginInterface
{
public:
    GlacierRelay();
    ~GlacierRelay() override;

    void Init() override;
    void OnEngineInitialized() override;

private:
    void OnFrameUpdate(const SGameUpdateEvent& p_UpdateEvent);
    void ObserveFrame();
    void ScheduleCommand(const SceneState& p_Scene, bool p_PlayingBefore);
    void ReportQueueDrops();
    void LogTelemetryCounters(const char* p_Reason);

    // M5 C-1 transition instrumentation (Relay-authored; never an engine value). AdvanceEpoch is
    // called on exactly these observations: a mission predicate rise, a fall, a contract.ended
    // observed for the active generation, the scene becoming unavailable during an attempt (once
    // per attempt), and a registry generation read on an inspection frame that differs from the
    // edge-cached one (once per attempt). It does not claim to see every engine transition.
    void AdvanceEpoch(const char* p_Why);
    void OnContractLifecycle(LifecycleTee::Kind p_Kind, const std::string& p_ContractSessionId);
    OutfitObservation::EngineReads InspectOutfit(const std::optional<std::string>& p_CandidateId);

    DECLARE_PLUGIN_DETOUR(
        GlacierRelay, void, OnTelemetryEventSent, ZAchievementManagerSimple* th, uint32_t eventIndex,
        const ZDynamicObject& event
    );

    enum class TelemetryLog
    {
        Off,   // only warnings
        Names, // one line per event the intake saw, with its decision (default)
    };

    // Intake-side counters, written on the detour's thread and read on the frame thread. Both were
    // the same thread in every run so far; atomics keep the counts honest if that ever changes.
    struct IntakeCounters
    {
        std::atomic<uint64_t> seen{0};
        std::atomic<uint64_t> captured{0};
        std::atomic<uint64_t> unsupported{0};
        std::atomic<uint64_t> dont_send{0};
        std::atomic<uint64_t> unreadable{0};
        std::atomic<uint64_t> truncated{0};
    };

private:
    bool m_FrameUpdateRegistered = false;
    bool m_LoggedUnavailable = false;
    SceneState m_LastScene;

    MissionObserver m_MissionObserver;
    std::unique_ptr<RelayAdapter> m_Adapter;

    TelemetryLog m_TelemetryLog = TelemetryLog::Names;
    TelemetryQueue m_TelemetryQueue{256};
    TelemetryNormalizer m_Normalizer;
    IntakeCounters m_Intake;
    uint64_t m_OutsideAttempt = 0;     // attempt-gated events observed with no open attempt (dropped)
    uint64_t m_UngatedPublished = 0;   // ungated (contract lifecycle) events published, any predicate state
    uint64_t m_DropsLogged = 0;        // queue drops already reported
    uint64_t m_DropWarnings = 0;

    // M5-B command path; all null when [command] enabled is false. Declared in this order so the
    // scheduler (which emits into the link) is destroyed before the link (which joins its thread)
    // and the link before the queue it drains.
    uint64_t m_Frame = 0;                              // the adapter's own frame counter
    std::optional<std::string> m_AttemptSessionId;     // the rise's game session id while an attempt is open
    bool m_CommandThreadLogged = false;
    std::unique_ptr<CommandQueue> m_CommandQueue;
    std::unique_ptr<CommandLink> m_CommandLink;
    std::unique_ptr<CommandScheduler> m_CommandScheduler;

    // M5 C-1 transition instrumentation (only maintained when the command link is enabled; the
    // tee is owned by the adapter's sink chain and this is a non-owning view for its stats).
    LifecycleTee* m_LifecycleTee = nullptr;
    uint64_t m_TransitionEpoch = 0;
    uint64_t m_RiseFrame = 0;
    uint64_t m_RiseMonoMs = 0;
    std::string m_RiseSceneResource;
    std::optional<std::string> m_ContractStartedSeen; // the last contract.started's session id
    std::optional<std::string> m_ContractEndedSeen;   // the last contract.ended's session id
    bool m_SceneUnavailableAdvanced = false;          // once per attempt
    bool m_RegistryChangeAdvanced = false;            // once per attempt
    uint64_t m_ContractEndsOtherGeneration = 0;       // contract.ended seen for a generation that is not the active one
};

DECLARE_ZHM_PLUGIN(GlacierRelay)
