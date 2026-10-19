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
#include "MotionController.h"
#include "MotionLink.h"
#include "OutfitApplier.h"
#include "RelayAdapter.h"
#include "SceneState.h"
#include "TelemetryNormalizer.h"
#include "TelemetryQueue.h"
#include "TransformApplier.h"

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
// schedules at most one per frame through CommandScheduler. relay.probe and relay.inspect.outfit
// write nothing. The telemetry sink and the detour are untouched by it; when disabled none of it
// is constructed.
//
// Effectful path (M5 C-2B): a second, separate setting, [command] effectful = false by default.
// Only with it true is the outfit applier constructed, player.outfit.set/2 advertised and
// decodable, and the one approved engine mutation (Functions::ZHitman5_SetOutfit, in
// OutfitApplier.cpp) reachable — behind the gate of OutfitApply on the applying frame. A run token
// ([command] token, 64 lowercase hex, never logged) makes the link speak protocol version 2 and
// attest; without it the link is version 1 exactly as C-1.
//
// Motion path (M7-B2; glacier-relay docs/design/M7_NETWORK_GHOST.md sections 15 and 17): a third,
// separate setting, [motion] enabled = false by default. With it on (and the run token), a third
// outbound loopback client (MotionLink, its own network thread, never a listener, never an engine
// read) binds to BEAM's motion listener for the current attested command link, and the frame update
// runs Motion::Controller after the command scheduler: the read-only share of the local hero
// (player.pose.share/1) and — only with [command] effectful on as well, which alone constructs the
// transform applier — the destination session (candidate.motion.session/1), whose per-frame checks
// on the native clock precede the one transform setter call site (EngineTransformApplier::Invoke).
// With it off nothing of it is constructed and the hello, the allowlist and every M6 path are M6's.
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

    // M6-B1: the scheduler's two read-only inspectors (relay.inspect.players / relay.inspect.entity),
    // guarded like InspectOutfit: a fault inside a read is logged and reported as unavailable.
    PlayerObservation::RegistryReads InspectPlayers();
    PlayerObservation::EntityReads InspectEntity(const PlayerObservation::EntityQuery& p_Query);

    // M6-D1a: the scheduler's read-only candidate inspector (relay.inspect.candidate), guarded the
    // same way.
    CandidateObservation::CandidateReads InspectCandidate(const CandidateObservation::CandidateQuery& p_Query);

    // M6-F: the scheduler's read-only exact-transform inspector (relay.inspect.candidate_transform),
    // guarded the same way.
    TransformObservation::Reads InspectCandidateTransform();

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
    std::unique_ptr<EngineOutfitApplier> m_OutfitApplier; // M5 C-2B; null unless [command] effectful = true
    std::unique_ptr<EngineTransformApplier> m_TransformApplier; // M6-F; null unless [command] effectful = true
    // M7-B2; both null unless [motion] enabled = true. Declared after the command link so they are
    // destroyed before it (the motion link's thread reads the command link's generation), the
    // controller after the link it holds a view of.
    std::unique_ptr<MotionLink> m_MotionLink;
    std::unique_ptr<Motion::Controller> m_MotionController;

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
