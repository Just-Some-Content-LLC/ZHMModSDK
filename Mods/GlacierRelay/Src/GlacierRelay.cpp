#include "GlacierRelay.h"

#include "Logging.h"
#include "ModSDKVersion.h"
#include "RelayLog.h"

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
    RelayLog::Info("plugin destroyed");
}

void GlacierRelay::Init()
{
    RelayLog::Info("Init: no hooks registered (the adapter only polls engine state)");
}

void GlacierRelay::OnEngineInitialized()
{
    RelayLog::Info("OnEngineInitialized");
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
