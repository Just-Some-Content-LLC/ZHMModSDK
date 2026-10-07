#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "IPluginInterface.h"

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
// open. No UI, no inbound network, no writes to engine state.
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
    void DrainTelemetry();
    void LogTelemetryCounters(const char* p_Reason);

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
    uint64_t m_OutsideAttempt = 0;     // normalized events observed with no open attempt (dropped)
    uint64_t m_DropsLogged = 0;        // queue drops already reported
    uint64_t m_DropWarnings = 0;
};

DECLARE_ZHM_PLUGIN(GlacierRelay)
