#include "GlacierRelay.h"

#include "Globals.h"
#include "Hooks.h"
#include "Logging.h"
#include "ModSDKVersion.h"
#include "Glacier/SGameUpdateEvent.h"
#include "Glacier/ZGameLoopManager.h"

#include "LogRelaySink.h"
#include "RelayEnvelope.h"
#include "RelayLog.h"
#include "SceneObservation.h"
#include "TcpRelaySink.h"
#include "RelayFrame.h"
#include "TelemetryIntake.h"

GlacierRelay::GlacierRelay()
{
    RelayLog::Info(
        "plugin constructed: instance {}, compiled against SDK {} (ABI {})",
        fmt::ptr(this), ZHMMODSDK_VER, ZHMMODSDK_ABI_VER
    );

    const auto s_LogPath = RelayLog::Path();

    if (s_LogPath.empty())
        Logger::Error("[GlacierRelay] Durable log could not be opened; only debugger output is available.");
    else
        Logger::Info("[GlacierRelay] Durable log: {}", s_LogPath);
}

GlacierRelay::~GlacierRelay()
{
    if (m_FrameUpdateRegistered)
    {
        const ZMemberDelegate<GlacierRelay, void(const SGameUpdateEvent&)> s_Delegate(this, &GlacierRelay::OnFrameUpdate);
        Globals::GameLoopManager->UnregisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdateAlways);
    }

    RelayLog::Info("plugin destroyed");
}

void GlacierRelay::Init()
{
    // The one hook: read-only, log-and-continue, on the engine's own telemetry stream (ADR 0006).
    Hooks::ZAchievementManagerSimple_OnEventSent->AddDetour(this, &GlacierRelay::OnTelemetryEventSent);
    RelayLog::Info("Init: one detour registered (ZAchievementManagerSimple_OnEventSent, read-only); lifecycle is polled");
}

void GlacierRelay::OnEngineInitialized()
{
    // The sink is chosen by the mod's settings file (Retail/mods/glacierrelay.ini):
    //   [relay] sink = tcp | log            (default tcp)
    //   [relay] port = 4747                 (loopback only; the host is not configurable)
    //   [relay] telemetry_log = names | off (default names: one line per telemetry event seen)
    // The adapter exists before the first frame update.
    const ZString s_SinkName = GetSetting("relay", "sink", "tcp");
    std::unique_ptr<IRelaySink> s_Sink;

    if (s_SinkName == "log")
    {
        s_Sink = std::make_unique<LogRelaySink>();
    }
    else
    {
        TcpRelaySink::Options s_Options;
        const auto s_Port = GetSettingInt("relay", "port", s_Options.port);

        if (s_Port > 0 && s_Port <= 65535)
            s_Options.port = static_cast<uint16_t>(s_Port);
        else
            RelayLog::Warn("relay port setting {} is out of range; using {}", s_Port, s_Options.port);

        s_Sink = std::make_unique<TcpRelaySink>(s_Options);
    }

    const ZString s_TelemetryLog = GetSetting("relay", "telemetry_log", "names");
    m_TelemetryLog = s_TelemetryLog == "off" ? TelemetryLog::Off : TelemetryLog::Names;

    m_Adapter = std::make_unique<RelayAdapter>(std::move(s_Sink), RelayAdapter::NewInstanceId(), &RelayAdapter::UtcNow);

    RelayLog::Info(
        "adapter instance {}, sink {}, protocol version {}, telemetry queue {} , telemetry_log {}",
        m_Adapter->InstanceId(), s_SinkName == "log" ? "LogRelaySink" : "TcpRelaySink", RelayProtocol::k_ProtocolVersion,
        m_TelemetryQueue.Capacity(), m_TelemetryLog == TelemetryLog::Off ? "off" : "names"
    );

    const ZMemberDelegate<GlacierRelay, void(const SGameUpdateEvent&)> s_Delegate(this, &GlacierRelay::OnFrameUpdate);
    Globals::GameLoopManager->RegisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdateAlways);
    m_FrameUpdateRegistered = true;

    RelayLog::Info("OnEngineInitialized: frame update registered (priority 1, eUpdateAlways)");
}

// The detour. Minimum work on the engine's thread: read the name and the policy flag, copy the
// bounded subset for supported events into owned memory, enqueue, continue. No normalization,
// serialization, logging of bodies or network work happens here.
DEFINE_PLUGIN_DETOUR(
    GlacierRelay, void, OnTelemetryEventSent, ZAchievementManagerSimple* th, uint32_t eventIndex,
    const ZDynamicObject& event
)
{
    RelayLog::Guard("OnTelemetryEventSent", [&] {
        ++m_Intake.seen;

        TelemetryObservation s_Observation;
        const auto s_Inspection = TelemetryIntake::Inspect(event, eventIndex, s_Observation);

        switch (s_Inspection.decision)
        {
            case TelemetryIntake::Decision::Captured:
                ++m_Intake.captured;
                if (s_Inspection.truncated)
                    ++m_Intake.truncated;
                m_TelemetryQueue.Push(std::move(s_Observation)); // a full queue counts the drop itself
                break;
            case TelemetryIntake::Decision::Unsupported:
                ++m_Intake.unsupported;
                break;
            case TelemetryIntake::Decision::DontSend:
                ++m_Intake.dont_send;
                break;
            case TelemetryIntake::Decision::Unreadable:
                ++m_Intake.unreadable;
                break;
        }

        if (m_TelemetryLog == TelemetryLog::Names)
        {
            const char* s_Decision = s_Inspection.decision == TelemetryIntake::Decision::Captured ? "captured"
                : s_Inspection.decision == TelemetryIntake::Decision::Unsupported ? "unsupported"
                : s_Inspection.decision == TelemetryIntake::Decision::DontSend ? "dont_send"
                : "unreadable";
            RelayLog::Info("telemetry seen (index {}): '{}' -> {}", eventIndex, s_Inspection.name, s_Decision);
        }
    });

    return HookResult<void>(HookAction::Continue());
}

void GlacierRelay::OnFrameUpdate(const SGameUpdateEvent& p_UpdateEvent)
{
    RelayLog::Guard("ObserveFrame", [&] { ObserveFrame(); });
}

// One observation per frame. The order inside RelayFrame::Process is a contract: telemetry
// queued since the last frame is drained and published against the attempt state that was
// authoritative when it was captured, and only then is this frame's scene observation fed to the
// mission observer and its edge published. See RelayFrame.h.
void GlacierRelay::ObserveFrame()
{
    ReportQueueDrops();

    const SceneState s_Scene = SceneObservation::ObserveScene();

    if (!s_Scene.available)
    {
        if (!m_LoggedUnavailable)
        {
            m_LoggedUnavailable = true;
            RelayLog::Error("scene state unavailable: scene context or application engine global is null");
        }

        // As before B1: no scene observation this frame; the observer keeps its state. Queued
        // telemetry is still judged against it below.
    }
    else if (s_Scene != m_LastScene)
    {
        RelayLog::Info(
            "scene: loaded {}, stage {}, type '{}', hint '{}', resource '{}'",
            s_Scene.scene_loaded, s_Scene.loading_stage, s_Scene.scene_type, s_Scene.codename_hint,
            s_Scene.scene_resource
        );

        m_LastScene = s_Scene;
    }

    // The session id is observational payload (it is Glacier's ContractSessionId, M2 design
    // section 19). It is read only on a frame an edge will fire, so the registry is not touched
    // every frame.
    std::optional<std::string> s_GameSessionId;

    if (s_Scene.available && m_MissionObserver.Playing() != MissionObserver::IsMissionPlaying(s_Scene))
        s_GameSessionId = SceneObservation::ObserveGameSessionId();

    const auto s_Frame = RelayFrame::Process(
        m_TelemetryQueue, m_Normalizer, m_MissionObserver, m_Adapter.get(),
        s_Scene.available ? std::optional<SceneState>(s_Scene) : std::nullopt, s_GameSessionId,
        [](const std::string& p_Line) { RelayLog::Warn("{}", p_Line); }
    );

    m_OutsideAttempt += s_Frame.outside_attempt;
    m_UngatedPublished += s_Frame.ungated_published;

    if (s_Frame.playing_before != s_Frame.playing_after)
    {
        RelayLog::Info("mission playing: {} -> {}", s_Frame.playing_before, s_Frame.playing_after);

        if (!s_Frame.playing_after)
            LogTelemetryCounters("attempt ended");
    }
}

// Queue drops are counted by the detour side; they are reported here, on the frame thread, with a
// rate limit.
void GlacierRelay::ReportQueueDrops()
{
    const auto s_Stats = m_TelemetryQueue.GetStats();

    if (s_Stats.dropped <= m_DropsLogged)
        return;

    if (m_DropWarnings < 8 || (s_Stats.dropped - m_DropsLogged) >= 100)
    {
        RelayLog::Warn(
            "telemetry queue full: {} observation(s) dropped so far (capacity {})", s_Stats.dropped,
            m_TelemetryQueue.Capacity()
        );
        ++m_DropWarnings;
        m_DropsLogged = s_Stats.dropped;
    }
}

void GlacierRelay::LogTelemetryCounters(const char* p_Reason)
{
    const auto& s_Norm = m_Normalizer.GetCounters();
    const auto s_Queue = m_TelemetryQueue.GetStats();

    RelayLog::Info(
        "telemetry counters ({}): seen {}, captured {}, unsupported {}, dont_send {}, unreadable {}, truncated {}; "
        "queue pushed {}, dropped {}; normalized {}, malformed {}, outside attempt {}, ungated published {}",
        p_Reason, m_Intake.seen.load(), m_Intake.captured.load(), m_Intake.unsupported.load(), m_Intake.dont_send.load(),
        m_Intake.unreadable.load(), m_Intake.truncated.load(), s_Queue.pushed, s_Queue.dropped, s_Norm.normalized,
        s_Norm.malformed, m_OutsideAttempt, m_UngatedPublished
    );
}

DEFINE_ZHM_PLUGIN(GlacierRelay);

BOOL WINAPI DllMain(HINSTANCE p_Module, DWORD p_Reason, LPVOID p_Reserved)
{
    if (p_Reason == DLL_PROCESS_ATTACH)
        RelayLog::ModuleAttached(p_Module);
    else if (p_Reason == DLL_PROCESS_DETACH)
        RelayLog::ModuleDetaching(p_Reserved != nullptr);

    return TRUE;
}
