#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "IPluginInterface.h"
#include "Glacier/ZEntity.h"

#include "MissionObserver.h"
#include "SceneState.h"

class ZActor;
class ZAchievementManagerSimple;
class ZDynamicObject;
class SOnlineEvent;
struct SGameUpdateEvent;

// M2 Stage B0 probe. Three observation surfaces, logged side by side so one occurrence can be read
// as a timeline:
//
//   S1  ZAchievementManagerSimple::OnEventSent / OnEventReceived   every event, raw, no filter, no parse
//   S2  Globals::ActorManager->m_activatedActors each frame           identity census, then only
//                                                                    IsAlive/IsDead/IsPacified edges
//   S3  SignalOutputPin                                               a fixed set of death/pacify pins,
//                                                                    emitter and order only
//
// Every detour logs and continues. Nothing is deduplicated, interpreted or written anywhere but the
// durable log. No engine state is mutated. Evidence, not architecture.
class ActorProbe : public IPluginInterface
{
public:
    ActorProbe();
    ~ActorProbe() override;

    void Init() override;
    void OnEngineInitialized() override;

private:
    void OnFrameUpdate(const SGameUpdateEvent& p_UpdateEvent);
    void ObserveFrame();
    void ScanActors();
    void LogCensus(const char* p_Reason);

    // What the probe remembers per activated actor. The pointer is the probe's own correlation key
    // and is never logged as an identity claim.
    struct ActorRecord
    {
        uint64_t entity_id = 0;
        uint64_t owner_entity_id = 0; // closest parent with a blueprint factory, 0 if none
        int32_t runtime_id = -1;
        std::string name;             // m_sActorName (authored label)
        std::string repository_name;  // GetActorName(): repository "Name" when resolvable
        std::string repository_id;    // entity property "RepositoryId", read once
        bool contract_target = false;
        bool crowd = false;
        bool alive = false;
        bool dead = false;
        bool pacified = false;
        bool seen_this_frame = false;
    };

    static ActorRecord Identify(ZActor* p_Actor);
    static std::string Describe(const ActorRecord& p_Record);

    DECLARE_PLUGIN_DETOUR(
        ActorProbe, void, OnEventSent, ZAchievementManagerSimple* th, uint32_t eventIndex, const ZDynamicObject& event
    );
    DECLARE_PLUGIN_DETOUR(ActorProbe, void, OnEventReceived, ZAchievementManagerSimple* th, const SOnlineEvent& event);
    DECLARE_PLUGIN_DETOUR(ActorProbe, bool, OnSignalOutputPin, ZEntityRef entity, uint32_t pinId, const ZObjectRef& data);

private:
    bool m_FrameUpdateRegistered = false;
    bool m_LoggedUnavailable = false;
    uint64_t m_Frame = 0;
    uint64_t m_EventsSent = 0;
    uint64_t m_EventsReceived = 0;
    uint64_t m_PinsLogged = 0;

    SceneState m_LastScene;
    MissionObserver m_Mission;
    std::unordered_map<ZActor*, ActorRecord> m_Actors;
};

DECLARE_ZHM_PLUGIN(ActorProbe)
