#include "ActorProbe.h"

#include <set>
#include <unordered_set>

#include "Functions.h"
#include "Globals.h"
#include "Hooks.h"
#include "Logging.h"
#include "ModSDKVersion.h"
#include "Glacier/Pins.h"
#include "Glacier/SGameUpdateEvent.h"
#include "Glacier/SOnlineEvent.h"
#include "Glacier/ZActor.h"
#include "Glacier/ZGameLoopManager.h"
#include "Glacier/ZObject.h"

#include "RelayLog.h"
#include "SceneObservation.h"

namespace
{
    // S3: the pins watched, by name. Everything else is ignored inside the detour so the probe does
    // not log the whole pin traffic of the game.
    struct WatchedPin
    {
        ZHMPin id;
        const char* name;
    };

    constexpr WatchedPin k_WatchedPins[] = {
        {ZHMPin::Dead, "Dead"},
        {ZHMPin::Death, "Death"},
        {ZHMPin::DeathContext, "DeathContext"},
        {ZHMPin::Pacified, "Pacified"},
        {ZHMPin::PacifiedData, "PacifiedData"},
        {ZHMPin::OnPacified, "OnPacified"},
        {ZHMPin::OnActorPacified, "OnActorPacified"},
        {ZHMPin::TargetKilled, "TargetKilled"},
        {ZHMPin::TargetPacified, "TargetPacified"},
        {ZHMPin::NonTargetKilled, "NonTargetKilled"},
        {ZHMPin::ActorKillIActor, "ActorKillIActor"},
        {ZHMPin::ActorPacifyIActor, "ActorPacifyIActor"},
        {ZHMPin::AccidentKill, "AccidentKill"},
        {ZHMPin::AllTargetsKilled, "AllTargetsKilled"},
    };

    const char* WatchedPinName(uint32_t p_PinId)
    {
        for (const auto& s_Pin : k_WatchedPins)
            if (static_cast<uint32_t>(s_Pin.id) == p_PinId)
                return s_Pin.name;

        return nullptr;
    }

    // At most this many new actors are identified (name, repository lookups) per frame; the rest
    // wait for the next frame. Keeps the first mission frame from doing hundreds of lookups at once.
    constexpr size_t k_IdentifyBudgetPerFrame = 50;

    uint64_t EntityIdOf(const ZEntityRef& p_Ref)
    {
        const auto* s_Entity = p_Ref.GetEntity();
        return s_Entity && s_Entity->GetType() ? s_Entity->GetType()->m_nEntityID : 0;
    }
}

ActorProbe::ActorProbe()
{
    RelayLog::Info(
        "actor probe constructed: instance {}, compiled against SDK {} (ABI {}). B0: S1 raw events, S2 actor census + "
        "IsAlive/IsDead/IsPacified edges, S3 {} watched pins. Log only.",
        fmt::ptr(this), ZHMMODSDK_VER, ZHMMODSDK_ABI_VER, std::size(k_WatchedPins)
    );

    const auto s_LogPath = RelayLog::Path();

    if (s_LogPath.empty())
        Logger::Error("[ActorProbe] Durable log could not be opened; only debugger output is available.");
    else
        Logger::Info("[ActorProbe] Durable log: {}", s_LogPath);
}

ActorProbe::~ActorProbe()
{
    if (m_FrameUpdateRegistered)
    {
        const ZMemberDelegate<ActorProbe, void(const SGameUpdateEvent&)> s_Delegate(this, &ActorProbe::OnFrameUpdate);
        Globals::GameLoopManager->UnregisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdateAlways);
    }

    RelayLog::Info("actor probe destroyed");
}

void ActorProbe::Init()
{
    Hooks::ZAchievementManagerSimple_OnEventSent->AddDetour(this, &ActorProbe::OnEventSent);
    Hooks::ZAchievementManagerSimple_OnEventReceived->AddDetour(this, &ActorProbe::OnEventReceived);
    Hooks::SignalOutputPin->AddDetour(this, &ActorProbe::OnSignalOutputPin);

    RelayLog::Info(
        "Init: detours registered on ZAchievementManagerSimple_OnEventSent, ZAchievementManagerSimple_OnEventReceived, "
        "SignalOutputPin (all log-and-continue)"
    );
}

void ActorProbe::OnEngineInitialized()
{
    const ZMemberDelegate<ActorProbe, void(const SGameUpdateEvent&)> s_Delegate(this, &ActorProbe::OnFrameUpdate);
    Globals::GameLoopManager->RegisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdateAlways);
    m_FrameUpdateRegistered = true;

    RelayLog::Info("OnEngineInitialized: frame update registered (priority 1, eUpdateAlways)");
}

void ActorProbe::OnFrameUpdate(const SGameUpdateEvent& p_UpdateEvent)
{
    RelayLog::Guard("ObserveFrame", [&] { ObserveFrame(); });
}

void ActorProbe::ObserveFrame()
{
    ++m_Frame;

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

    // The same predicate the relay uses, so the probe's timeline aligns with mission.playing /
    // mission.stopped. The probe publishes nothing.
    const bool s_WasPlaying = m_Mission.Playing();
    const auto s_Edge = m_Mission.Update(s_Scene, std::nullopt);

    if (m_Mission.Playing() != s_WasPlaying)
        RelayLog::Info("mission playing: {} -> {} (frame {})", s_WasPlaying, m_Mission.Playing(), m_Frame);

    ScanActors();

    if (s_Edge && m_Mission.Playing())
        LogCensus("mission playing");
}

ActorProbe::ActorRecord ActorProbe::Identify(ZActor* p_Actor)
{
    ActorRecord s_Record;

    ZEntityRef s_Ref;
    p_Actor->GetID(s_Ref);

    s_Record.entity_id = EntityIdOf(s_Ref);
    s_Record.owner_entity_id = EntityIdOf(s_Ref.GetClosestParentWithBlueprintFactory());
    s_Record.runtime_id = p_Actor->m_nActorRuntimeId;
    s_Record.name = std::string(p_Actor->m_sActorName.ToStringView());
    s_Record.repository_name = std::string(p_Actor->GetActorName().ToStringView());
    s_Record.contract_target = p_Actor->m_bContractTarget;
    s_Record.crowd = p_Actor->m_bCrowdCharacter;

    auto s_RepositoryId = s_Ref.GetProperty<ZRepositoryID>("RepositoryId");
    s_Record.repository_id = std::string(s_RepositoryId.Get().ToString().ToStringView());

    return s_Record;
}

std::string ActorProbe::Describe(const ActorRecord& p_Record)
{
    return fmt::format(
        "entity {:016x} owner {:016x} runtime {} repo {} name '{}' repo-name '{}' target {} crowd {}",
        p_Record.entity_id, p_Record.owner_entity_id, p_Record.runtime_id, p_Record.repository_id, p_Record.name,
        p_Record.repository_name, p_Record.contract_target, p_Record.crowd
    );
}

// S2. One pass over the activated list: new actors are identified (budgeted) and logged with their
// initial state; known actors are logged only when IsAlive/IsDead/IsPacified change; actors that
// left the list are logged as gone (which is not a death claim).
void ActorProbe::ScanActors()
{
    if (!Globals::ActorManager)
        return;

    for (auto& s_Pair : m_Actors)
        s_Pair.second.seen_this_frame = false;

    size_t s_IdentifiedThisFrame = 0;
    const auto& s_Activated = Globals::ActorManager->m_activatedActors;

    for (size_t i = 0; i < s_Activated.size(); ++i)
    {
        ZActor* s_Actor = s_Activated[i].m_pInterfaceRef;

        if (!s_Actor)
            continue;

        const bool s_Alive = s_Actor->IsAlive();
        const bool s_Dead = s_Actor->IsDead();
        const bool s_Pacified = s_Actor->IsPacified();

        auto s_It = m_Actors.find(s_Actor);

        if (s_It == m_Actors.end())
        {
            if (s_IdentifiedThisFrame >= k_IdentifyBudgetPerFrame)
                continue;

            ++s_IdentifiedThisFrame;

            ActorRecord s_Record = Identify(s_Actor);
            s_Record.alive = s_Alive;
            s_Record.dead = s_Dead;
            s_Record.pacified = s_Pacified;
            s_Record.seen_this_frame = true;

            RelayLog::Info(
                "actor + (frame {}) {} | alive {} dead {} pacified {} | ptr {}",
                m_Frame, Describe(s_Record), s_Alive, s_Dead, s_Pacified, fmt::ptr(s_Actor)
            );

            m_Actors.emplace(s_Actor, std::move(s_Record));
            continue;
        }

        ActorRecord& s_Record = s_It->second;
        s_Record.seen_this_frame = true;

        if (s_Alive != s_Record.alive || s_Dead != s_Record.dead || s_Pacified != s_Record.pacified)
        {
            RelayLog::Info(
                "actor state (frame {}) entity {:016x} runtime {} repo {} name '{}' target {}: alive {} -> {}, dead {} -> {}, "
                "pacified {} -> {} | ptr {}",
                m_Frame, s_Record.entity_id, s_Record.runtime_id, s_Record.repository_id, s_Record.name,
                s_Record.contract_target, s_Record.alive, s_Alive, s_Record.dead, s_Dead, s_Record.pacified,
                s_Pacified, fmt::ptr(s_Actor)
            );

            s_Record.alive = s_Alive;
            s_Record.dead = s_Dead;
            s_Record.pacified = s_Pacified;
        }
    }

    for (auto s_It = m_Actors.begin(); s_It != m_Actors.end();)
    {
        if (s_It->second.seen_this_frame)
        {
            ++s_It;
            continue;
        }

        RelayLog::Info(
            "actor - (frame {}) entity {:016x} runtime {} name '{}' left the activated list (last alive {} dead {} pacified {}) | ptr {}",
            m_Frame, s_It->second.entity_id, s_It->second.runtime_id, s_It->second.name, s_It->second.alive,
            s_It->second.dead, s_It->second.pacified, fmt::ptr(s_It->first)
        );

        s_It = m_Actors.erase(s_It);
    }
}

// Identity census over what the probe has identified so far. Duplicate authored ids are the H8
// open question; they are listed, not resolved.
void ActorProbe::LogCensus(const char* p_Reason)
{
    std::unordered_set<uint64_t> s_EntityIds, s_RuntimeIds;
    std::set<std::string> s_RepositoryIds;
    std::unordered_map<uint64_t, int> s_EntityIdCounts;
    size_t s_Targets = 0, s_Crowd = 0, s_Alive = 0, s_Dead = 0, s_Pacified = 0;

    for (const auto& [s_Ptr, s_Record] : m_Actors)
    {
        s_EntityIds.insert(s_Record.entity_id);
        s_RuntimeIds.insert(static_cast<uint64_t>(static_cast<int64_t>(s_Record.runtime_id)));
        s_RepositoryIds.insert(s_Record.repository_id);
        ++s_EntityIdCounts[s_Record.entity_id];
        s_Targets += s_Record.contract_target;
        s_Crowd += s_Record.crowd;
        s_Alive += s_Record.alive;
        s_Dead += s_Record.dead;
        s_Pacified += s_Record.pacified;
    }

    const size_t s_ActivatedNow = Globals::ActorManager ? Globals::ActorManager->m_activatedActors.size() : 0;
    const size_t s_TargetList = Globals::ActorManager ? Globals::ActorManager->m_aTargetList.size() : 0;

    RelayLog::Info(
        "census ({}, frame {}): identified {} of {} activated; distinct entity ids {}, distinct runtime ids {}, distinct "
        "repository ids {}; targets {} (manager target list {}), crowd {}; alive {} dead {} pacified {}",
        p_Reason, m_Frame, m_Actors.size(), s_ActivatedNow, s_EntityIds.size(), s_RuntimeIds.size(),
        s_RepositoryIds.size(), s_Targets, s_TargetList, s_Crowd, s_Alive, s_Dead, s_Pacified
    );

    int s_Listed = 0;

    for (const auto& [s_Id, s_Count] : s_EntityIdCounts)
    {
        if (s_Count < 2)
            continue;

        if (++s_Listed > 20)
        {
            RelayLog::Info("census: further duplicate entity ids not listed");
            break;
        }

        std::string s_Names;

        for (const auto& [s_Ptr, s_Record] : m_Actors)
            if (s_Record.entity_id == s_Id)
                s_Names += fmt::format("[owner {:016x} runtime {} '{}'] ", s_Record.owner_entity_id, s_Record.runtime_id, s_Record.name);

        RelayLog::Info("census: entity id {:016x} appears {} times: {}", s_Id, s_Count, s_Names);
    }

    if (s_Listed == 0)
        RelayLog::Info("census: no duplicate entity ids among identified actors");
}

// S1. The complete event, raw. No filter, no parse, no dedup: multiplicity is something to measure.
DEFINE_PLUGIN_DETOUR(
    ActorProbe, void, OnEventSent, ZAchievementManagerSimple* th, uint32_t eventIndex, const ZDynamicObject& event
)
{
    RelayLog::Guard("OnEventSent", [&] {
        ZString s_Json;
        Functions::ZDynamicObject_ToString->Call(const_cast<ZDynamicObject*>(&event), s_Json);

        ++m_EventsSent;
        RelayLog::Info("S1 sent #{} (index {}, frame {}): {}", m_EventsSent, eventIndex, m_Frame, s_Json.ToStringView());
    });

    return HookResult<void>(HookAction::Continue());
}

DEFINE_PLUGIN_DETOUR(ActorProbe, void, OnEventReceived, ZAchievementManagerSimple* th, const SOnlineEvent& event)
{
    RelayLog::Guard("OnEventReceived", [&] {
        ZString s_Json;
        Functions::ZDynamicObject_ToString->Call(const_cast<ZDynamicObject*>(&event.data), s_Json);

        ++m_EventsReceived;
        RelayLog::Info(
            "S1 received #{} (frame {}): '{}' {}", m_EventsReceived, m_Frame, event.sName.ToStringView(), s_Json.ToStringView()
        );
    });

    return HookResult<void>(HookAction::Continue());
}

// S3. Only the watched pins; emitter identity and whether it is an actor the probe knows.
DEFINE_PLUGIN_DETOUR(ActorProbe, bool, OnSignalOutputPin, ZEntityRef entity, uint32_t pinId, const ZObjectRef& data)
{
    const char* s_PinName = WatchedPinName(pinId);

    if (!s_PinName)
        return HookResult<bool>(HookAction::Continue());

    RelayLog::Guard("OnSignalOutputPin", [&] {
        const uint64_t s_EntityId = EntityIdOf(entity);
        const uint64_t s_OwnerId = EntityIdOf(entity.GetClosestParentWithBlueprintFactory());
        ZActor* s_Actor = entity.QueryInterface<ZActor>();

        std::string s_ActorText = "not an actor";

        if (s_Actor)
        {
            const auto s_It = m_Actors.find(s_Actor);
            s_ActorText = s_It != m_Actors.end()
                ? fmt::format("actor runtime {} name '{}' ptr {}", s_It->second.runtime_id, s_It->second.name, fmt::ptr(s_Actor))
                : fmt::format("actor (not yet identified) ptr {}", fmt::ptr(s_Actor));
        }

        ++m_PinsLogged;
        RelayLog::Info(
            "S3 pin #{} (frame {}) '{}' (id {}) from entity {:016x} owner {:016x}: {}",
            m_PinsLogged, m_Frame, s_PinName, static_cast<int32_t>(pinId), s_EntityId, s_OwnerId, s_ActorText
        );
    });

    return HookResult<bool>(HookAction::Continue());
}

DEFINE_ZHM_PLUGIN(ActorProbe);

BOOL WINAPI DllMain(HINSTANCE p_Module, DWORD p_Reason, LPVOID p_Reserved)
{
    if (p_Reason == DLL_PROCESS_ATTACH)
        RelayLog::ModuleAttached(p_Module);
    else if (p_Reason == DLL_PROCESS_DETACH)
        RelayLog::ModuleDetaching(p_Reserved != nullptr);

    return TRUE;
}
