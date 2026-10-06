#include "Hitmen.h"

#include "Hooks.h"
#include "Logging.h"

#include <Glacier/ZScene.h>

#include "IconsMaterialDesign.h"
#include "Glacier/ZGeomEntity.h"
#include "Glacier/ZModule.h"
#include "Glacier/ZSpatialEntity.h"
#include "Glacier/ZCameraEntity.h"
#include "Glacier/ZRender.h"
#include "Glacier/EntityFactory.h"
#include <ranges>

#include <imgui_impl_dx12.h>
#include "Glacier/SGameUpdateEvent.h"
#include "Glacier/ZCollision.h"
#include "Glacier/ZPhysics.h"
#include "Glacier/ZGameLoopManager.h"
#include "Glacier/ZHitman5.h"
#include "Glacier/ZApplicationEngineWin32.h"
#include "Glacier/ZEngineAppCommon.h"
#include "Glacier/ZPlayerRegistry.h"
#include "Glacier/ZActor.h"

#include "BinaryStreamReader.h"
#include "BinaryStreamWriter.h"
#include "HitmenLog.h"

Hitmen::Hitmen()
{
    HitmenLog::Info(
        "plugin constructed: instance {}, compiled against SDK {} (ABI {})",
        fmt::ptr(this), ZHMMODSDK_VER, ZHMMODSDK_ABI_VER
    );

    const auto s_LogPath = HitmenLog::Path();

    if (s_LogPath.empty())
        Logger::Error("[Hitmen] Durable log could not be opened; only debugger output is available.");
    else
        Logger::Info("[Hitmen] Durable log: {}", s_LogPath);
}

Hitmen::~Hitmen()
{
    HitmenLog::Info("plugin destructor enter: unregistering frame update");

    const ZMemberDelegate<Hitmen, void(const SGameUpdateEvent&)> s_Delegate(this, &Hitmen::OnFrameUpdate);
    Globals::GameLoopManager->UnregisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdateAlways);

    HitmenLog::Info("plugin destructor exit");
}

void Hitmen::OnEngineInitialized()
{
    HitmenLog::Info("OnEngineInitialized enter");

    m_Transport = std::make_unique<NullHitmenTransport>();

    HitmenLog::Info("transport: NullHitmenTransport (no sockets; inbound delivery impossible)");

    const ZMemberDelegate<Hitmen, void(const SGameUpdateEvent&)> s_Delegate(this, &Hitmen::OnFrameUpdate);
    Globals::GameLoopManager->RegisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdateAlways);

    m_Initialized = true;

    HitmenLog::Info("OnEngineInitialized exit: frame update registered (priority 1, eUpdateAlways)");
}

void Hitmen::Init()
{
    // AddDetour reports nothing, and whether the SDK found and patched the engine function is
    // only in the SDK's own log. Proof that a detour is live is its first "enter" line.
    HitmenLog::Info(
        "Init enter: registering detours on ClearScene hook {} and LoadScene hook {}",
        fmt::ptr(Hooks::ZEntitySceneContext_ClearScene), fmt::ptr(Hooks::ZEntitySceneContext_LoadScene)
    );

    Hooks::ZEntitySceneContext_ClearScene->AddDetour(this, &Hitmen::OnClearScene);
    Hooks::ZEntitySceneContext_LoadScene->AddDetour(this, &Hitmen::OnLoadScene);

    HitmenLog::Info("Init exit: OnClearScene and OnLoadScene detours registered");
}

void Hitmen::StartServer(uint16_t p_Port)
{
    if (!m_Transport->StartServer(p_Port))
    {
        Logger::Error("[Hitmen] Failed to start server.");
        return;
    }

    Logger::Info("[Hitmen] Hitmen server started on port {}.", p_Port);
    m_IsServer = true;
}

void Hitmen::Connect(const std::string& p_Address, uint16_t p_Port)
{
    if (!m_Transport->Connect(p_Address, p_Port))
    {
        Logger::Error("[Hitmen] Could not create client connection.");
        return;
    }
}

void Hitmen::UpdateConnection()
{
    const auto s_Connection = m_Transport->PollConnected();

    if (s_Connection == k_InvalidHitmenConnection)
        return;

    // TODO: Multiple clients.
    m_ClientConnection = s_Connection;
    m_Connected = true;

    if (!m_IsServer)
        m_IsClient = true;
}

enum MessageId
{
    InputsAndPositions,
    NpcPositions,
};

// H3: the 2023 Hitman transform/input sync is excluded from the dormant build, not ported.
// Its receive side writes remote transforms and raw input bytes into the game. Kept verbatim
// as protocol evidence (glacier-relay docs/research/HITMEN_COMPILE_ARCHAEOLOGY.md).
#if 0
void Hitmen::ProcessMessages()
{
    for (const auto& s_Msg : m_Transport->ReceiveMessages())
    {
        //Logger::Debug("[Hitmen] Got message with {} bytes.", s_Msg.m_Data.size());

        // TODO: This is extremely incredibly unsafe
        BinaryStreamReader s_Reader(s_Msg.m_Data.data(), s_Msg.m_Data.size());

        switch (s_Reader.Read<MessageId>())
        {
            case InputsAndPositions:
                OnInputsAndPosition(s_Reader);
                break;

            case NpcPositions:
                OnNpcPositions(s_Reader);
                break;
        }
    }
}

void Hitmen::SendInputsAndPosition(HitmenConnection p_Connection)
{
    BinaryStreamWriter s_Writer(sizeof(SMatrix) + 0x148);

    // TODO: This is extremely incredibly unsafe
    s_Writer.Write(InputsAndPositions);
    s_Writer.Write(m_OurHitman.QueryInterface<ZSpatialEntity>()->GetWorldMatrix());
    s_Writer.WriteBinary(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(m_OurHitman.QueryInterface<ZHitman5>()->m_pCharacterInputProcessor->m_pInput) + sizeof(uintptr_t)), 0x148);

    m_Transport->SendUnreliable(p_Connection, s_Writer.Buffer(), s_Writer.WrittenBytes());
}
#endif

// H8: the 2023 NPC position sync is excluded from the dormant build, not ported. It identifies
// an NPC across machines by its index in ZActorManager::m_aActiveActors, a layout that no longer
// exists. Kept verbatim as protocol evidence (glacier-relay docs/research/HITMEN_ENTITY_IDENTITY.md).
#if 0
void Hitmen::SendNpcPositions(HitmenConnection p_Connection)
{
    BinaryStreamWriter s_Writer(8192);

    uint32_t s_AliveActorCount = 0;

    for (int i = 0; i < *Globals::NextActorId; ++i)
    {
        auto* s_Actor = Globals::ActorManager->m_aActiveActors[i].m_pInterfaceRef;

        if (s_Actor->IsAlive())
            ++s_AliveActorCount;
    }

    s_Writer.Write(NpcPositions);
    s_Writer.Write(s_AliveActorCount);

    for (int i = 0; i < *Globals::NextActorId; ++i)
    {
        const auto& s_Actor = Globals::ActorManager->m_aActiveActors[i];

        if (s_Actor.m_pInterfaceRef->IsAlive())
        {
            s_Writer.Write(i);
            s_Writer.Write(s_Actor.m_entityRef.QueryInterface<ZSpatialEntity>()->GetWorldMatrix());
        }
    }

    m_Transport->SendUnreliable(p_Connection, s_Writer.Buffer(), s_Writer.WrittenBytes());
}
#endif

// H3: excluded from the dormant build (see ProcessMessages above).
#if 0
void Hitmen::OnInputsAndPosition(BinaryStreamReader& p_Reader)
{
    const auto& s_Position = p_Reader.Read<SMatrix>();

    if (m_OtherHitman)
    {
        m_OtherHitman.SetProperty("m_eRoomBehaviour", ZSpatialEntity::ERoomBehaviour::ROOM_DYNAMIC);

        const auto s_SpatialHitman = m_OtherHitman.QueryInterface<ZSpatialEntity>();

        /*auto s_CurrentPos = s_SpatialHitman->GetWorldMatrix();

        if (float4::Distance(s_CurrentPos.Pos, s_Position.Pos) >= 0.3f)
        {
            s_CurrentPos.Pos = s_Position.Pos;
            s_SpatialHitman->SetWorldMatrix(s_CurrentPos);
        }*/

        s_SpatialHitman->SetWorldMatrix(s_Position);

        const auto s_OtherHitman = m_OtherHitman.QueryInterface<ZHitman5>();

        if (s_OtherHitman->m_pCharacterInputProcessor && s_OtherHitman->m_pCharacterInputProcessor->m_pInput)
        {
            // TODO: This is extremely incredibly unsafe
            p_Reader.ReadBytes(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(s_OtherHitman->m_pCharacterInputProcessor->m_pInput) + sizeof(uintptr_t)), 0x148);
        }
    }
}
#endif

// H8: excluded from the dormant build (see SendNpcPositions above).
#if 0
void Hitmen::OnNpcPositions(BinaryStreamReader& p_Reader)
{
    const auto s_Actors = p_Reader.Read<uint32_t>();

    for (int i = 0; i < s_Actors; ++i)
    {
        // TODO: This is extremely incredibly unsafe
        const auto& s_ActorIndex = p_Reader.Read<int>();
        const auto& s_ActorPos = p_Reader.Read<SMatrix>();

        if (s_ActorIndex < *Globals::NextActorId)
        {
            const auto& s_Actor = Globals::ActorManager->m_aActiveActors[s_ActorIndex];

            if (s_Actor.m_pInterfaceRef->IsAlive())
            {
                s_Actor.m_entityRef.QueryInterface<ZSpatialEntity>()->SetWorldMatrix(s_ActorPos);
            }
        }
    }
}
#endif

void Hitmen::OnFrameUpdate(const SGameUpdateEvent& p_UpdateEvent)
{
    /*m_UpdateTimer += p_UpdateEvent.m_RealTimeDelta.ToSeconds();
    m_NpcUpdateTimer += p_UpdateEvent.m_RealTimeDelta.ToSeconds();

    if (!m_Initialized)
        return;

    UpdateConnection();
    ProcessMessages();

    if (!m_Connected)
        return;*/

    HitmenLog::Guard("ObserveSceneState", [&] { ObserveSceneState(); });

    auto s_Scene = Globals::Hitman5Module->m_pEntitySceneContext->m_pScene;

    if (!s_Scene || !(*Globals::ApplicationEngineWin32)->m_bSceneLoaded)
        return;

    HitmenLog::Guard("ObserveLocalPlayer", [&] { ObserveLocalPlayer(p_UpdateEvent); });

    if (!m_SceneLoaded)
    {
        auto s_LocalHitman = SDK()->GetLocalPlayer();

        if (!s_LocalHitman)
            return;

        const auto s_HitmanSpatial = s_LocalHitman.m_entityRef.QueryInterface<ZSpatialEntity>();

        if (!s_HitmanSpatial)
            return;

        m_OurHitman = s_LocalHitman.m_entityRef;

        for (auto& s_Brick : Globals::Hitman5Module->m_pEntitySceneContext->m_aLoadedBricks)
        {
            if (s_Brick.m_RuntimeResourceID != ResId<"[assembly:/_sdk/hitmen.brick].pc_entitytype">)
                continue;

            const auto s_BpFactory = reinterpret_cast<ZTemplateEntityBlueprintFactory*>(s_Brick.m_EntityRef.GetBlueprintFactory());

            if (!s_BpFactory)
                continue;

            if (const auto s_Index = s_BpFactory->GetSubEntityIndex(0xfeede715906f747f); s_Index != -1)
            {
                m_OtherHitman = s_BpFactory->GetSubEntity(s_Brick.m_EntityRef.m_pObj, s_Index);
            }

            if (m_OtherHitman)
            {
                Logger::Debug(
					"[Hitmen] Found other hitman {} (base {}) and our hitman {} (base {}).",
                    fmt::ptr(m_OtherHitman.QueryInterface<ZHitman5>()),
                    fmt::ptr(m_OtherHitman.GetEntity()),
                    fmt::ptr(m_OurHitman.QueryInterface<ZHitman5>()),
                    fmt::ptr(m_OurHitman.GetEntity())
                );
                m_SceneLoaded = true;
            }

            break;
        }

        return;
    }

    //if (m_UpdateTimer >= 1.f / 30.f)
    /*{
        m_UpdateTimer = 0.f;
        SendInputsAndPosition(m_ClientConnection);
    }

    if (m_NpcUpdateTimer >= 1.f / 10.f)
    {
        m_NpcUpdateTimer = 0.f;

        if (m_IsServer)
            SendNpcPositions(m_ClientConnection);
    }*/
}

// Logs the scene state the legacy logic in OnFrameUpdate polls, whenever it changes. Read-only.
void Hitmen::ObserveSceneState()
{
    const auto* s_Context = Globals::Hitman5Module ? Globals::Hitman5Module->m_pEntitySceneContext : nullptr;
    const auto* s_Engine = Globals::ApplicationEngineWin32 ? *Globals::ApplicationEngineWin32 : nullptr;

    if (!s_Context || !s_Engine)
    {
        if (!m_ObservedMissingGlobals)
        {
            m_ObservedMissingGlobals = true;
            HitmenLog::Error(
                "scene state unavailable: scene context {}, application engine {}",
                fmt::ptr(s_Context), fmt::ptr(s_Engine)
            );
        }

        return;
    }

    const bool s_SceneLoaded = s_Context->m_pScene && s_Engine->m_bSceneLoaded;
    const auto s_LoadingStage = static_cast<int32_t>(s_Context->m_LoadingStage);

    if (s_SceneLoaded == m_ObservedSceneLoaded && s_LoadingStage == m_ObservedLoadingStage)
        return;

    // m_LoadingStage (0x178) has no static_assert and no other user in the SDK, so treat the
    // stage number as unverified until a run shows it stepping through ESceneLoadingStage.
    HitmenLog::Info(
        "scene state: loaded {} -> {}, loading stage {} -> {} (unverified offset), scene '{}'",
        m_ObservedSceneLoaded, s_SceneLoaded, m_ObservedLoadingStage, s_LoadingStage,
        s_Context->m_SceneInitParameters.m_SceneResource
    );

    m_ObservedSceneLoaded = s_SceneLoaded;
    m_ObservedLoadingStage = s_LoadingStage;
}

// Logs local-player resolution once per scene, then samples the local Hitman's world transform
// every few seconds. Read-only: the same lookups the legacy logic in OnFrameUpdate performs.
void Hitmen::ObserveLocalPlayer(const SGameUpdateEvent& p_UpdateEvent)
{
    const auto s_LocalHitman = SDK()->GetLocalPlayer();
    const auto* s_Spatial = s_LocalHitman ? s_LocalHitman.m_entityRef.QueryInterface<ZSpatialEntity>() : nullptr;

    if (!s_Spatial)
    {
        if (!m_ObservedLocalPlayerMissing)
        {
            m_ObservedLocalPlayerMissing = true;
            HitmenLog::Info(
                "local player not resolved in this scene yet: ZHitman5 {}, ZSpatialEntity {}",
                fmt::ptr(s_LocalHitman.m_pInterfaceRef), fmt::ptr(s_Spatial)
            );
        }

        return;
    }

    if (!m_ObservedLocalPlayer)
    {
        m_ObservedLocalPlayer = true;
        m_TransformSampleTimer = 0.f;

        HitmenLog::Info(
            "local player resolved: ZHitman5 {}, entity {}, ZSpatialEntity {}",
            fmt::ptr(s_LocalHitman.m_pInterfaceRef), fmt::ptr(s_LocalHitman.m_entityRef.GetEntity()),
            fmt::ptr(s_Spatial)
        );

        size_t s_BrickCount = 0;
        bool s_HitmenBrickLoaded = false;

        for (const auto& s_Brick : Globals::Hitman5Module->m_pEntitySceneContext->m_aLoadedBricks)
        {
            ++s_BrickCount;

            if (s_Brick.m_RuntimeResourceID == ResId<"[assembly:/_sdk/hitmen.brick].pc_entitytype">)
                s_HitmenBrickLoaded = true;
        }

        // The second Hitman is content from hitmen.brick, not something this dll creates.
        HitmenLog::Info("loaded bricks: {}, hitmen.brick loaded: {}", s_BrickCount, s_HitmenBrickLoaded);

        DumpPlayerRegistry("local player resolved");
    }
    else
    {
        m_TransformSampleTimer += static_cast<float>(p_UpdateEvent.m_RealTimeDelta.ToSeconds());

        if (m_TransformSampleTimer < 5.f)
            return;

        m_TransformSampleTimer = 0.f;
    }

    // GetObjectToWorldMatrix refreshes the entity's cached world matrix when the engine has marked
    // it dirty, exactly as it does for every other caller. Nothing is written to the transform.
    const SMatrix s_Transform = s_Spatial->GetObjectToWorldMatrix();

    HitmenLog::Info(
        "local transform read: position ({:.3f}, {:.3f}, {:.3f})",
        s_Transform.Trans.x, s_Transform.Trans.y, s_Transform.Trans.z
    );
}

// ZStrings in registry slots come from a reverse-engineered layout, so report the raw length and
// pointer and only read the characters when the length is plausible.
static std::string DescribeString(const ZString& p_String)
{
    if (p_String.size() > 256 || (p_String.size() != 0 && !p_String.c_str()))
        return fmt::format("<{} chars at {}, not read>", p_String.size(), fmt::ptr(p_String.c_str()));

    return fmt::format("'{}'", p_String.ToStringView());
}

// Dumps ZPlayerRegistry to the durable log. Read-only.
void Hitmen::DumpPlayerRegistry(const char* p_Trigger)
{
    const auto* s_Registry = Globals::PlayerRegistry;

    HitmenLog::Info("registry: dump begin ({}), ZPlayerRegistry {}", p_Trigger, fmt::ptr(s_Registry));

    if (!s_Registry)
    {
        HitmenLog::Error("registry: Globals::PlayerRegistry is null, nothing to dump");
        return;
    }

    // What the current SDK models at 0x390, where the 2023 code read m_pLocalPlayer.
    const auto& s_Players = s_Registry->m_PlayerData;

    HitmenLog::Info(
        "registry: m_PlayerData {} entries, begin {}, end {}, allocation end {}",
        s_Players.size(), fmt::ptr(s_Players.m_pBegin), fmt::ptr(s_Players.m_pEnd),
        fmt::ptr(s_Players.m_pAllocationEnd)
    );

    // Whether the array is a view over the four inline slots or separate storage is not known.
    if (s_Players.size() <= 16)
    {
        for (size_t i = 0; i < s_Players.size(); ++i)
        {
            const auto* s_Entry = &s_Players.m_pBegin[i];
            const ptrdiff_t s_Slot = s_Entry - &s_Registry->m_aPlayerData[0];
            const bool s_Inline = s_Entry >= &s_Registry->m_aPlayerData[0] && s_Entry < &s_Registry->m_aPlayerData[4];

            HitmenLog::Info(
                "registry: m_PlayerData[{}] at {}, inline slot {}",
                i, fmt::ptr(s_Entry), s_Inline ? std::to_string(s_Slot) : "none (outside m_aPlayerData)"
            );
        }
    }
    else
    {
        HitmenLog::Warn("registry: m_PlayerData size is implausible, entries not walked");
    }

    for (int i = 0; i < 4; ++i)
    {
        const auto& s_Data = s_Registry->m_aPlayerData[i];

        /*
         * class ZNetPlayerController :
            public ZBaseReplica
        {
        public:
            void* m_unk0x8; // 0x10 (-8)
            uint32_t m_nFlags0x10; // 0x18 (-8)
            void* m_unk0x18; // 0x20 (-8)
            uint32_t m_nFlags0x20; // 0x28 (-8)
            void* m_nFlags0x28; // 0x30 (-8)
            bool m_bUnk0x30; // 0x38 (-8)
            uint32_t m_nFlags0x34; // 0x3C (-8)
            uint32_t m_nFlags0x38; // 0x40 (-8)
            uint32_t m_nFlags0x3C; // 0x44 (-8)
            uint16_t m_nFlags0x40; // 0x48 (-8)
            bool m_bUnk0x42; // 0x4A (-8)
            void* m_unk0x48; // 0x50 (-8)
            void* m_unk0x50; // 0x58 (-8)
            void* m_unk0x58; // 0x60 (-8)
            ZString m_sUnk0x60; // 0x68 (-8)
            uint32_t m_nFlags0x70; // 0x78 (-8)
            void* m_unk0x78; // 0x80 (-8)
            void* m_unk0x80; // 0x88 (-8)
            ZString m_sPlayerOnlineId; // 0x90 (-8)
            uint32_t m_nFlags0x98; // 0xA0 (-8)
            ZEntityRef m_HitmanEntity; // 0xA8 (-8)
            void* m_unk0xA8; // 0xB0 (-8) a pointer to the entity vtables, probably something related to the aspect dummy
            void* m_unk0xB0; // 0xB8 (-8)
            void* m_unk0xB8; // 0xC0 (-8)
            void* m_unk0xC0; // 0xC8 (-8)
        };
         */

        HitmenLog::Info("registry: slot [{}] at {}", i, fmt::ptr(&s_Data));
        HitmenLog::Info("registry: [{}] player id = {}", i, s_Data.m_nPlayerId);
        HitmenLog::Info("registry: [{}] counter = {}", i, s_Data.m_Controller.m_nUnkCounter);
        HitmenLog::Info("registry: [{}] flags 0x18 = {:08X}", i, s_Data.m_Controller.m_nFlags0x10);
        HitmenLog::Info("registry: [{}] raknet replica = {}", i, fmt::ptr(s_Data.m_Controller.m_pRakNetReplica));
        HitmenLog::Info("registry: [{}] flags 0x28 = {:08X}", i, s_Data.m_Controller.m_nFlags0x20);
        HitmenLog::Info("registry: [{}] flags 0x30 = {}", i, fmt::ptr(s_Data.m_Controller.m_nFlags0x28));
        HitmenLog::Info("registry: [{}] is local player = {}", i, s_Data.m_Controller.m_bLocalPlayer);
        HitmenLog::Info("registry: [{}] flags 0x3C = {:08X}", i, s_Data.m_Controller.m_nFlags0x34);
        HitmenLog::Info("registry: [{}] flags 0x40 = {:08X}", i, s_Data.m_Controller.m_nFlags0x38);
        HitmenLog::Info("registry: [{}] flags 0x44 = {:08X}", i, s_Data.m_Controller.m_nFlags0x3C);
        HitmenLog::Info("registry: [{}] flags 0x48 = {:04X}", i, s_Data.m_Controller.m_nFlags0x40);
        HitmenLog::Info("registry: [{}] connected = {}", i, s_Data.m_Controller.m_bConnectedToMultiplayer);
        HitmenLog::Info("registry: [{}] net player = {}", i, fmt::ptr(s_Data.m_Controller.m_pNetPlayer));
        HitmenLog::Info("registry: [{}] character id = {}", i, s_Data.m_Controller.m_SelectedCharacterId.ToString());
        HitmenLog::Info("registry: [{}] string 0x68 = {}", i, DescribeString(s_Data.m_Controller.m_sUnk0x60));
        HitmenLog::Info("registry: [{}] flags 0x78 = {:08X}", i, s_Data.m_Controller.m_nFlags0x70);
        HitmenLog::Info("registry: [{}] outfit id = {}", i, s_Data.m_Controller.m_OutfitId.ToString());
        HitmenLog::Info("registry: [{}] player session id (?) = {}", i, DescribeString(s_Data.m_Controller.s_sSessionId));
        HitmenLog::Info("registry: [{}] flags 0xA0 = {:08X}", i, s_Data.m_Controller.m_nFlags0x98);
        HitmenLog::Info("registry: [{}] hitman entity = {}", i, fmt::ptr(s_Data.m_Controller.m_HitmanEntity.GetEntity()));
        HitmenLog::Info("registry: [{}] entity vtables = {}", i, fmt::ptr(s_Data.m_Controller.m_pEntityVtables));
        HitmenLog::Info("registry: [{}] unk 0xB8 = {}", i, fmt::ptr(s_Data.m_Controller.m_unk0xB0));
        HitmenLog::Info("registry: [{}] unk 0xC0 = {}", i, fmt::ptr(s_Data.m_Controller.m_unk0xB8));
        HitmenLog::Info("registry: [{}] unk 0xC8 = {}", i, fmt::ptr(s_Data.m_Controller.m_unk0xC0));
    }

    const auto s_LocalHitman = SDK()->GetLocalPlayer();

    HitmenLog::Info(
        "registry: SDK local player: ZHitman5 {}, entity {}",
        fmt::ptr(s_LocalHitman.m_pInterfaceRef), fmt::ptr(s_LocalHitman.m_entityRef.GetEntity())
    );

    HitmenLog::Info("registry: dump end");
}

void Hitmen::OnDrawMenu()
{
    if (ImGui::Button("Player registry"))
    {
        HitmenLog::Guard("DumpPlayerRegistry (menu button)", [&] { DumpPlayerRegistry("menu button"); });
        Logger::Info("[Hitmen] Player registry dumped to {}", HitmenLog::Path());
    }

    if (ImGui::Button("Hitmen"))
    {
        m_ShowServerWindow = !m_ShowServerWindow;
    }
}

void Hitmen::OnDrawUI(bool p_HasFocus)
{
    auto s_ImgGuiIO = ImGui::GetIO();

    if (m_ShowServerWindow)
    {
        if (ImGui::Begin("Host hitmen server", &m_ShowServerWindow))
        {
            ImGui::Text("Server port = 6969");

            if (ImGui::Button("Start server"))
            {
                StartServer(6969);
                m_ShowServerWindow = false;
            }
        }

        ImGui::End();
    }

    if (m_ShowClientWindow)
    {
        if (ImGui::Begin("Connect to hitmen server", &m_ShowClientWindow))
        {
            ImGui::Text("Server port = 6969");

            static char s_ServerAddr[1024] = {};
            ImGui::InputText("Server address", s_ServerAddr, IM_ARRAYSIZE(s_ServerAddr));

            if (ImGui::Button("Connect"))
            {
                Connect(s_ServerAddr, 6969);
                m_ShowClientWindow = false;
            }
        }

        ImGui::End();
    }
}

void Hitmen::OnDraw3D(IRenderer* p_Renderer)
{
    /*if (m_OtherHitman)
    {
        if (auto* s_SpatialEntity = m_OtherHitman.QueryInterface<ZSpatialEntity>())
        {
            SMatrix s_Transform;
            Functions::ZSpatialEntity_WorldTransform->Call(s_SpatialEntity, &s_Transform);

            float4 s_Min, s_Max;

            s_SpatialEntity->CalculateBounds(s_Min, s_Max, 1, 0);

            p_Renderer->DrawOBB3D(SVector3(s_Min.x, s_Min.y, s_Min.z), SVector3(s_Max.x, s_Max.y, s_Max.z), s_Transform, SVector4(0.f, 0.f, 1.f, 1.f));
        }
    }*/
}

DEFINE_PLUGIN_DETOUR(Hitmen, bool, OnLoadScene, ZEntitySceneContext* th, SSceneInitParameters& p_SceneData)
{
    HitmenLog::Guard("OnLoadScene", [&] {
        HitmenLog::Info(
            "OnLoadScene enter: context {}, scene '{}', type '{}', codename hint '{}', start game {}, {} additional bricks",
            fmt::ptr(th), p_SceneData.m_SceneResource, p_SceneData.m_Type, p_SceneData.m_CodeNameHint,
            p_SceneData.m_bStartGame, p_SceneData.m_aAdditionalBrickResources.size()
        );

        for (const auto& s_Brick : p_SceneData.m_aAdditionalBrickResources)
            HitmenLog::Info("OnLoadScene brick: '{}'", s_Brick);
    });

    // p_SceneData.m_sceneName = "assembly:/_pro/scenes/users/notex/test.entity";
    //p_SceneData.m_sceneName = "assembly:/_pro/scenes/missions/golden/mission_gecko/scene_gecko_basic.entity";
    //p_SceneData.m_sceneName = "assembly:/_PRO/Scenes/Missions/TheFacility/_Scene_Mission_Polarbear_Module_002_B.entity";
    //p_SceneData.m_sceneBricks.clear();

    //p_SceneData.m_sceneName = "assembly:/_PRO/Scenes/Missions/Ancestral/scene_bulldog.entity";
    //p_SceneData.m_sceneBricks.clear();
    //p_SceneData.m_sceneBricks.push_back("assembly:/_PRO/scenes/missions/golden/mission_gecko/mission_gecko.brick");

    /*
     * Loading scene: assembly:/_pro/scenes/missions/golden/mission_gecko/scene_gecko_basic.entity
+ With brick: assembly:/_PRO/scenes/missions/golden/mission_gecko/mission_gecko.brick
     */
    HitmenLog::Info("OnLoadScene exit: continuing to the original");
    return HookResult<bool>(HookAction::Continue());
}

DEFINE_PLUGIN_DETOUR(Hitmen, void, OnClearScene, ZEntitySceneContext* th, bool p_FullyUnloadScene)
{
    HitmenLog::Info(
        "OnClearScene enter: context {}, fully unload {}, second Hitman was found {}",
        fmt::ptr(th), p_FullyUnloadScene, static_cast<bool>(m_OtherHitman)
    );

    m_OtherHitman = {};
    m_FirstHitman = {};
    m_SceneLoaded = false;

    m_ObservedLocalPlayer = false;
    m_ObservedLocalPlayerMissing = false;

    HitmenLog::Info("OnClearScene exit: continuing to the original");
    return HookResult<void>(HookAction::Continue());
}

DEFINE_ZHM_PLUGIN(Hitmen);

BOOL WINAPI DllMain(HINSTANCE p_Module, DWORD p_Reason, LPVOID p_Reserved)
{
    if (p_Reason == DLL_PROCESS_ATTACH)
        HitmenLog::ModuleAttached(p_Module);
    else if (p_Reason == DLL_PROCESS_DETACH)
        HitmenLog::ModuleDetaching(p_Reserved != nullptr);

    return TRUE;
}
