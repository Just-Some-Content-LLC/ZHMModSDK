#pragma once

#include <memory>
#include <random>
#include <unordered_map>

#include "IPluginInterface.h"
#include "Glacier/ZEntity.h"
#include "Glacier/ZInput.h"
#include "HitmenTransport.h"

class BinaryStreamReader;
class ZHitman5;

class Hitmen : public IPluginInterface
{
public:
    Hitmen();
    ~Hitmen() override;

    void OnEngineInitialized() override;
    void Init() override;
    void OnDrawMenu() override;
    void OnDrawUI(bool p_HasFocus) override;
    void OnDraw3D(IRenderer* p_Renderer) override;

private:
    void OnFrameUpdate(const SGameUpdateEvent& p_UpdateEvent);
    void StartServer(uint16_t p_Port);
    void Connect(const std::string& p_Address, uint16_t p_Port);
    void UpdateConnection();

    // The 2023 sync functions below are declared but their definitions are excluded from the
    // dormant build (#if 0 in Hitmen.cpp), so any call to them fails at link time.
    void ProcessMessages();

    void SendInputsAndPosition(HitmenConnection p_Connection);
    void SendNpcPositions(HitmenConnection p_Connection);

    void OnInputsAndPosition(BinaryStreamReader& p_Reader);
    void OnNpcPositions(BinaryStreamReader& p_Reader);

private:
    DECLARE_PLUGIN_DETOUR(Hitmen, bool, OnLoadScene, ZEntitySceneContext*, SSceneInitParameters&);
    DECLARE_PLUGIN_DETOUR(Hitmen, void, OnClearScene, ZEntitySceneContext* th, bool p_FullyUnloadScene);

private:
    bool m_Initialized = false;
    ZEntityRef m_OtherHitman;
    ZEntityRef m_OurHitman;
    TEntityRef<ZHitman5> m_FirstHitman;
    std::unique_ptr<IHitmenTransport> m_Transport;
    HitmenConnection m_ClientConnection = k_InvalidHitmenConnection;
    bool m_SceneLoaded = false;
    bool m_IsServer = false;
    bool m_IsClient = false;

    bool m_ShowServerWindow = false;
    bool m_ShowClientWindow = false;

    bool m_Connected = false;
    float m_UpdateTimer = 0.f;
    float m_NpcUpdateTimer = 0.f;
};

DECLARE_ZHM_PLUGIN(Hitmen)
