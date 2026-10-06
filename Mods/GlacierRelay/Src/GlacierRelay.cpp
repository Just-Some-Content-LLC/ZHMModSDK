#include "GlacierRelay.h"

#include "Globals.h"
#include "Logging.h"
#include "ModSDKVersion.h"
#include "Glacier/SGameUpdateEvent.h"
#include "Glacier/ZGameLoopManager.h"

#include "LogRelaySink.h"
#include "RelayEnvelope.h"
#include "RelayLog.h"
#include "SceneObservation.h"

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
    RelayLog::Info("Init: no hooks registered (the adapter only polls engine state)");
}

void GlacierRelay::OnEngineInitialized()
{
    // Stage 1: the only sink is the durable log. The adapter exists before the first frame update.
    m_Adapter = std::make_unique<RelayAdapter>(
        std::make_unique<LogRelaySink>(), RelayAdapter::NewInstanceId(), &RelayAdapter::UtcNow
    );

    RelayLog::Info("adapter instance {}, sink LogRelaySink, protocol version {}", m_Adapter->InstanceId(), RelayProtocol::k_ProtocolVersion);

    const ZMemberDelegate<GlacierRelay, void(const SGameUpdateEvent&)> s_Delegate(this, &GlacierRelay::OnFrameUpdate);
    Globals::GameLoopManager->RegisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdateAlways);
    m_FrameUpdateRegistered = true;

    RelayLog::Info("OnEngineInitialized: frame update registered (priority 1, eUpdateAlways)");
}

void GlacierRelay::OnFrameUpdate(const SGameUpdateEvent& p_UpdateEvent)
{
    RelayLog::Guard("ObserveFrame", [&] { ObserveFrame(); });
}

// One observation per frame: read scene state, log changes, feed the semantic layer, publish on
// the mission.playing edge.
void GlacierRelay::ObserveFrame()
{
    const SceneState s_Scene = SceneObservation::ObserveScene();

    if (!s_Scene.available)
    {
        if (!m_LoggedUnavailable)
        {
            m_LoggedUnavailable = true;
            RelayLog::Error("scene state unavailable: scene context or application engine global is null");
        }

        return;
    }

    if (s_Scene != m_LastScene)
    {
        RelayLog::Info(
            "scene: loaded {}, stage {}, type '{}', hint '{}', resource '{}'",
            s_Scene.scene_loaded, s_Scene.loading_stage, s_Scene.scene_type, s_Scene.codename_hint,
            s_Scene.scene_resource
        );

        m_LastScene = s_Scene;
    }

    // The session id is observational payload. It is read only on the frame the edge will fire,
    // so the registry is not touched every frame.
    std::optional<std::string> s_GameSessionId;

    if (!m_MissionObserver.Playing() && MissionObserver::IsMissionPlaying(s_Scene))
        s_GameSessionId = SceneObservation::ObserveGameSessionId();

    const bool s_WasPlaying = m_MissionObserver.Playing();
    const auto s_Event = m_MissionObserver.Update(s_Scene, s_GameSessionId);

    if (m_MissionObserver.Playing() != s_WasPlaying)
        RelayLog::Info("mission playing: {} -> {}", s_WasPlaying, m_MissionObserver.Playing());

    if (!s_Event)
        return;

    if (!m_Adapter)
    {
        RelayLog::Error("mission.playing edge observed before the adapter existed; event dropped");
        return;
    }

    m_Adapter->Publish(*s_Event);
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
