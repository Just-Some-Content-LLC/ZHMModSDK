#include "GlacierRelay.h"

#include <Windows.h>

#include <chrono>

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

    // M5 C-1: with the command link enabled, the sink is wrapped by a tee that reports the contract
    // lifecycle envelopes' session ids to the transition instrumentation (forwarding every envelope
    // unchanged). With it disabled the sink chain is exactly the M2 one.
    const bool s_CommandEnabled = GetSettingBool("command", "enabled", false);

    if (s_CommandEnabled)
    {
        auto s_Tee = std::make_unique<LifecycleTee>(std::move(s_Sink), [this](LifecycleTee::Kind p_Kind, const std::string& p_Id) {
            OnContractLifecycle(p_Kind, p_Id);
        });
        m_LifecycleTee = s_Tee.get();
        s_Sink = std::move(s_Tee);
    }

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

    // M5-B: the command link, only when the mod's settings ask for it:
    //   [command] enabled = false (default)   no thread, no connection, no queue, nothing constructed
    //   [command] port = 4749                 loopback only; the host is not configurable
    if (s_CommandEnabled)
    {
        CommandLink::Options s_Command;
        const auto s_CommandPort = GetSettingInt("command", "port", s_Command.port);

        if (s_CommandPort > 0 && s_CommandPort <= 65535)
            s_Command.port = static_cast<uint16_t>(s_CommandPort);
        else
            RelayLog::Warn("command port setting {} is out of range; using {}", s_CommandPort, s_Command.port);

        // M5 C-2B: the effectful setting and the run token, both separate from [command] enabled.
        //   [command] effectful = false (default)  the outfit applier is not constructed; player.outfit.set/2
        //                                          is neither advertised nor decodable
        //   [command] token = <64 lowercase hex>   protocol version 2 with attestation; never logged
        s_Command.effectful = GetSettingBool("command", "effectful", false);
        const ZString s_Token = GetSetting("command", "token", "");
        s_Command.token = std::string(s_Token.ToStringView());

        if (s_Command.effectful)
        {
            m_OutfitApplier = std::make_unique<EngineOutfitApplier>();
            m_TransformApplier = std::make_unique<EngineTransformApplier>(); // M6-F
        }

        m_CommandQueue = std::make_unique<CommandQueue>();
        m_CommandLink = std::make_unique<CommandLink>(s_Command, *m_CommandQueue, m_Adapter->InstanceId(), [] {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        });
        m_CommandScheduler = std::make_unique<CommandScheduler>(*m_CommandQueue, [this](const CommandProtocol::Result& p_Result) {
            RelayLog::Info("command: {} {} on frame {}", p_Result.command_id, CommandProtocol::StatusText(p_Result.status), p_Result.frame ? *p_Result.frame : 0);
            m_CommandLink->PushResult(p_Result);
        });

        if (s_Command.effectful)
            RelayLog::Info("command link enabled: port {}, allowlist relay.probe/1, relay.inspect.outfit/1, relay.inspect.players/1, relay.inspect.entity/1, relay.inspect.candidate/1, relay.inspect.candidate_transform/1 (read-only), player.outfit.set/2 (EFFECTFUL: one guarded ZHitman5_SetOutfit call behind the applying-frame gate), candidate.transform.set/1 (EFFECTFUL: lift | restore) and candidate.transform.set/2 (EFFECTFUL: step_y | restore; one guarded SetObjectToWorldMatrixFromEditor call on the compiled candidate behind the applying-frame gate and the one-forward ledger shared by both entries), one command per frame; protocol version {}", s_Command.port, m_CommandLink->ProtocolVersion());
        else
            RelayLog::Info("command link enabled: port {}, allowlist relay.probe/1, relay.inspect.outfit/1, relay.inspect.players/1, relay.inspect.entity/1, relay.inspect.candidate/1 and relay.inspect.candidate_transform/1 (read-only), one command per frame, no engine write ([command] effectful = false); protocol version {}", s_Command.port, m_CommandLink->ProtocolVersion());
    }
    else
    {
        RelayLog::Info("command link disabled ([command] enabled = false, the default)");
    }
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

        // The attempt's session id (Glacier's ContractSessionId as observed on the rise) is the
        // candidate mission generation of M5-B: it is held while the attempt is open and absent
        // otherwise. It is Glacier's identity for the attempt, never derived here.
        m_AttemptSessionId = s_Frame.playing_after ? s_GameSessionId : std::nullopt;

        if (m_CommandLink)
        {
            m_CommandLink->SetMissionGeneration(m_AttemptSessionId);

            // M5 C-1: every predicate edge advances the Relay transition epoch; a rise records
            // its frame, clock and scene resource and resets the once-per-attempt markers.
            AdvanceEpoch(s_Frame.playing_after ? "mission predicate rise" : "mission predicate fall");

            if (s_Frame.playing_after)
            {
                m_RiseFrame = m_Frame + 1; // the number this frame will carry below
                m_RiseMonoMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
                m_RiseSceneResource = s_Scene.available ? s_Scene.scene_resource : std::string();
                m_SceneUnavailableAdvanced = false;
                m_RegistryChangeAdvanced = false;
            }
        }

        if (!s_Frame.playing_after)
            LogTelemetryCounters("attempt ended");
    }
    else if (m_CommandLink && m_AttemptSessionId && !s_Scene.available && !m_SceneUnavailableAdvanced)
    {
        m_SceneUnavailableAdvanced = true;
        AdvanceEpoch("scene state unavailable during an attempt");
    }

    ++m_Frame;

    if (m_CommandScheduler)
        ScheduleCommand(s_Scene, s_Frame.playing_before);
}

// M5-B: at most one queued command per frame, judged on this frame's observation. relay.probe
// writes nothing; its read-back is the plain values above. Runs after the frame's own
// observation and publication so a command never reorders the telemetry sequence.
void GlacierRelay::ScheduleCommand(const SceneState& p_Scene, bool p_PlayingBefore)
{
    CommandScheduler::FrameObservation s_Observation;
    s_Observation.frame = m_Frame;
    s_Observation.now_mono_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    s_Observation.native_at = RelayAdapter::UtcNow();
    s_Observation.thread_id = GetCurrentThreadId();
    s_Observation.scene_available = p_Scene.available;
    s_Observation.scene = p_Scene;
    s_Observation.mission_playing = m_MissionObserver.Playing();
    s_Observation.mission_generation = m_AttemptSessionId;
    s_Observation.link_generation = m_CommandLink->LinkGeneration();
    s_Observation.adapter_instance_id = m_Adapter->InstanceId();

    // M5 C-1: the transition instrumentation as plain values, and the inspector the scheduler may
    // call on this frame for relay.inspect.outfit only (never on an idle frame).
    s_Observation.relay_transition_epoch = m_TransitionEpoch;
    s_Observation.previous_frame_playing = p_PlayingBefore;
    s_Observation.rise_frame = m_RiseFrame;
    s_Observation.rise_mono_ms = m_RiseMonoMs;
    s_Observation.scene_resource_same_as_rise = p_Scene.available && p_Scene.scene_resource == m_RiseSceneResource;
    s_Observation.contract_started_seen = m_ContractStartedSeen;
    s_Observation.contract_ended_seen = m_ContractEndedSeen;
    s_Observation.inspector = [this](const std::optional<std::string>& p_CandidateId) { return InspectOutfit(p_CandidateId); };

    // M6-B1: the two read-only inspectors, called by the scheduler on this frame only for the
    // command taken this frame (never on an idle frame).
    s_Observation.players_inspector = [this] { return InspectPlayers(); };
    s_Observation.entity_inspector = [this](const PlayerObservation::EntityQuery& p_Query) { return InspectEntity(p_Query); };

    // M6-D1a: the read-only candidate inspector, called by the scheduler on this frame only for a
    // relay.inspect.candidate command taken this frame (never on an idle frame).
    s_Observation.candidate_inspector = [this](const CandidateObservation::CandidateQuery& p_Query) { return InspectCandidate(p_Query); };

    // M6-F: the read-only exact-transform inspector (called by the scheduler on this frame only for
    // a relay.inspect.candidate_transform command taken this frame) and the transform applier,
    // only when the effectful setting constructed it.
    s_Observation.transform_inspector = [this] { return InspectCandidateTransform(); };
    s_Observation.transform_applier = m_TransformApplier.get();

    // M5 C-2B: the applier only when the effectful setting constructed it; the link generation
    // re-read immediately before any setter call.
    s_Observation.applier = m_OutfitApplier.get();
    s_Observation.link_generation_now = [this] { return m_CommandLink->LinkGeneration(); };

    if (!m_CommandThreadLogged)
    {
        m_CommandThreadLogged = true;
        RelayLog::Info("command scheduler: first frame on thread {} (network thread {})", s_Observation.thread_id, m_CommandLink->GetStats().network_thread_id);
    }

    m_CommandScheduler->Tick(s_Observation);
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

// ---- M5 C-1: transition instrumentation and the frame-local inspection ------------------------

void GlacierRelay::AdvanceEpoch(const char* p_Why)
{
    ++m_TransitionEpoch;
    RelayLog::Info("relay transition epoch {}: {} (frame {})", m_TransitionEpoch, p_Why, m_Frame);
}

// Called by the lifecycle tee on the frame thread while a contract envelope is published.
void GlacierRelay::OnContractLifecycle(LifecycleTee::Kind p_Kind, const std::string& p_ContractSessionId)
{
    if (p_Kind == LifecycleTee::Kind::ContractStarted)
    {
        m_ContractStartedSeen = p_ContractSessionId;
        return;
    }

    m_ContractEndedSeen = p_ContractSessionId;

    if (m_AttemptSessionId && *m_AttemptSessionId == p_ContractSessionId)
        AdvanceEpoch("contract end observed for the active generation");
    else
        ++m_ContractEndsOtherGeneration;
}

// The scheduler's inspector: the guarded engine reads of this frame, under their own fault guard
// so a fault inside a read is logged with the command's context and reported as unavailable
// rather than ending the frame. A registry generation that differs from the edge-cached one is
// itself transition evidence and advances the epoch (once per attempt) for the frames after it.
OutfitObservation::EngineReads GlacierRelay::InspectOutfit(const std::optional<std::string>& p_CandidateId)
{
    OutfitObservation::EngineReads s_Reads;
    bool s_Completed = false;

    RelayLog::Guard("InspectOutfit", [&] {
        s_Reads = SceneObservation::ObserveOutfit(p_CandidateId);
        s_Completed = true;
    });

    if (!s_Completed)
    {
        OutfitObservation::EngineReads s_Faulted;
        s_Faulted.thread_id = GetCurrentThreadId();
        s_Faulted.generation_fresh = OutfitObservation::Field<std::string>::Of(CommandProtocol::FieldStatus::Unavailable, "fault during the inspection reads");
        s_Faulted.hero.outfit_id = OutfitObservation::Field<std::string>::Of(CommandProtocol::FieldStatus::Unavailable, "fault during the inspection reads");
        s_Faulted.hero.heroa_valid = OutfitObservation::Field<bool>::Of(CommandProtocol::FieldStatus::Unavailable, "fault during the inspection reads");
        s_Faulted.set_outfit_available = OutfitObservation::Field<bool>::Of(CommandProtocol::FieldStatus::Unavailable, "fault during the inspection reads");
        s_Faulted.kit_map_available = OutfitObservation::Field<bool>::Of(CommandProtocol::FieldStatus::Unavailable, "fault during the inspection reads");
        return s_Faulted;
    }

    if (m_AttemptSessionId && s_Reads.generation_fresh.observed() && *s_Reads.generation_fresh.value != *m_AttemptSessionId && !m_RegistryChangeAdvanced)
    {
        m_RegistryChangeAdvanced = true;
        AdvanceEpoch("registry generation differs from the edge-cached generation on an inspection frame");
    }

    return s_Reads;
}

// M6-B1: the registry inspection under its own fault guard; a fault inside a read is logged with
// the command's context and reported as unavailable rather than ending the frame.
PlayerObservation::RegistryReads GlacierRelay::InspectPlayers()
{
    PlayerObservation::RegistryReads s_Reads;
    bool s_Completed = false;

    RelayLog::Guard("InspectPlayers", [&] {
        s_Reads = SceneObservation::ObservePlayers();
        s_Completed = true;
    });

    if (!s_Completed)
    {
        auto s_Faulted = PlayerObservation::AllRegistry(CommandProtocol::FieldStatus::Unavailable, "fault during the registry reads");
        s_Faulted.thread_id = GetCurrentThreadId();
        return s_Faulted;
    }

    return s_Reads;
}

// M6-B1: the entity inspection under its own fault guard, as above.
PlayerObservation::EntityReads GlacierRelay::InspectEntity(const PlayerObservation::EntityQuery& p_Query)
{
    PlayerObservation::EntityReads s_Reads;
    bool s_Completed = false;

    RelayLog::Guard("InspectEntity", [&] {
        s_Reads = SceneObservation::ObserveEntity(p_Query);
        s_Completed = true;
    });

    if (!s_Completed)
    {
        auto s_Faulted = PlayerObservation::AllEntity(CommandProtocol::FieldStatus::Unavailable, "fault during the entity reads", p_Query);
        s_Faulted.thread_id = GetCurrentThreadId();
        return s_Faulted;
    }

    return s_Reads;
}

// M6-D1a: the candidate inspection under its own fault guard, as above.
CandidateObservation::CandidateReads GlacierRelay::InspectCandidate(const CandidateObservation::CandidateQuery& p_Query)
{
    CandidateObservation::CandidateReads s_Reads;
    bool s_Completed = false;

    RelayLog::Guard("InspectCandidate", [&] {
        s_Reads = SceneObservation::ObserveCandidate(p_Query);
        s_Completed = true;
    });

    if (!s_Completed)
    {
        auto s_Faulted = CandidateObservation::AllCandidate(CommandProtocol::FieldStatus::Unavailable, "candidate reads faulted", p_Query);
        s_Faulted.thread_id = GetCurrentThreadId();
        return s_Faulted;
    }

    return s_Reads;
}

// M6-F: the exact-transform inspection under its own fault guard, as above.
TransformObservation::Reads GlacierRelay::InspectCandidateTransform()
{
    TransformObservation::Reads s_Reads;
    bool s_Completed = false;

    RelayLog::Guard("InspectCandidateTransform", [&] {
        s_Reads = SceneObservation::ObserveCandidateTransform();
        s_Completed = true;
    });

    if (!s_Completed)
    {
        auto s_Faulted = TransformObservation::AllUnavailable(CommandProtocol::FieldStatus::Unavailable, "transform reads faulted");
        s_Faulted.thread_id = GetCurrentThreadId();
        return s_Faulted;
    }

    return s_Reads;
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
