#include "privatehook.h"
#include "soundcache_control.h"
#include <cvardef.h>
#include <demo_api.h>
#include <net_api.h>
#include <initializer_list>

static_assert(METAHOOK_API_VERSION >= svensoundcachecontrol::RequiredMetaHookAPIVersion,
              "SvenSoundcacheControl requires gamedata symbol resolution (MetaHook API 109)");

namespace svensoundcachecontrol
{
namespace
{
using ClientCmdFn = decltype(cl_enginefunc_t::pfnClientCmd);
using FlushAction = SoundcacheControl::FlushAction;

constexpr const char* SvenClientInterface    = "SCClientDLL001";
constexpr const char* MapStartedSymbol       = "CClient_SoundEngine_MapStarted";
constexpr const char* ClearSoundArraysSymbol = "CClient_SoundEngine_ClearSoundArrays";
constexpr const char* FlushCacheSymbol       = "CClient_SoundEngine_FlushCache";
constexpr const char* LoadSoundListSymbol    = "CClient_SoundEngine_LoadSoundList";
constexpr const char* ClientEngfuncsSymbol   = "gEngfuncs";
constexpr const char* AlwaysFlushCvarName    = "cl_soundcache_alwaysflush";

hook_t*            mapStartedHook        = nullptr;
hook_t*            flushCacheHook        = nullptr;
hook_t*            loadSoundListHook     = nullptr;
MapStartedFn       originalMapStarted    = nullptr;
FlushCacheFn       originalFlushCache    = nullptr;
LoadSoundListFn    originalLoadSoundList = nullptr;
ClearSoundArraysFn clearSoundArrays      = nullptr;

// The client DLL's own engine table; the loader issues its commands through it.
cl_enginefunc_t* clientEngfuncs    = nullptr;
ClientCmdFn      originalClientCmd = nullptr;

// The engine keeps calling the wrapped exports after hooks are removed, so the
// originals stay valid until the client is loaded again.
HUD_INIT_FUNC     originalHudInit     = nullptr;
HUD_SHUTDOWN_FUNC originalHudShutdown = nullptr;

cvar_t*           alwaysFlush      = nullptr;
bool              loadingSoundList = false;
SoundcacheControl control;

void ReportFailure(const char* symbol, const char* reason)
{
    uint64_t   crc64  = 0;
    const auto status = g_pMetaHookAPI->GetModuleCRC64(g_pMetaHookAPI->GetClientBase(), &crc64);
    if (status == MH_GAMESYMBOL_OK)
        g_pMetaHookAPI->SysError("[SvenSoundcacheControl] %s (module client)\nEngine buildnum: %d\nCRC64: %016llx\nReason: %s",
                                 symbol, g_pMetaHookAPI->GetEngineBuildnum(), static_cast<unsigned long long>(crc64), reason);
    else
        g_pMetaHookAPI->SysError("[SvenSoundcacheControl] %s (module client)\nEngine buildnum: %d\nReason: %s\nCRC64 unavailable: %s",
                                 symbol, g_pMetaHookAPI->GetEngineBuildnum(), reason, g_pMetaHookAPI->GetGameSymbolStatusString(status));
}

bool Resolve(const char* symbol, mh_gamesymbol_kind_t kind, void** address)
{
    const auto status = g_pMetaHookAPI->ResolveGameSymbol(g_pMetaHookAPI->GetClientBase(), symbol, kind, address);
    if (status == MH_GAMESYMBOL_OK)
        return true;
    ReportFailure(symbol, g_pMetaHookAPI->GetGameSymbolStatusString(status));
    return false;
}

bool IsSvenClient()
{
    const auto factory = g_pMetaHookAPI->GetClientFactory();
    return factory && factory(SvenClientInterface, nullptr);
}

bool IsDemoPlayback()
{
    const auto demo = gEngfuncs.pDemoAPI;
    return demo && demo->IsPlayingback && demo->IsPlayingback();
}

bool IsEnabled()
{
    return alwaysFlush && alwaysFlush->value != 0 && !IsDemoPlayback();
}

// Identify the join so a recovery reconnect can be told apart from a new one.
void BeginMap()
{
    net_status_t status = {};
    const auto   net    = gEngfuncs.pNetAPI;
    if (net && net->Status)
        net->Status(&status);
    const char* server = net && net->AdrToString ? net->AdrToString(&status.remote_address) : nullptr;
    const char* map    = gEngfuncs.pfnGetLevelName();
    control.BeginMap(map ? map : "", server ? server : "", status.remote_address.type == NA_LOOPBACK);
}

// The original engine clears its in-memory indices only when the map name changes.
void __fastcall MapStartedHook(void* self, int edx)
{
    BeginMap();
    if (IsEnabled())
    {
        // A queued dlfile from the previous load may have recreated the list.
        // Invalidate once per join, allowing the SDK's recovery join to use it.
        if (control.ShouldInvalidateAtMapStart())
            originalFlushCache(self, edx, true);
        clearSoundArrays(self, edx);
    }
    originalMapStarted(self, edx);
}

void __fastcall FlushCacheHook(void* self, int edx, bool includingLocal)
{
    switch (control.Flush(IsEnabled(), includingLocal, loadingSoundList))
    {
        case FlushAction::Forward:
            originalFlushCache(self, edx, includingLocal);
            break;
        case FlushAction::ForceAll:
            originalFlushCache(self, edx, true);
            break;
        case FlushAction::Preserve:
            break;
    }
}

// A missing list makes the loader queue dlfile and request `retry`. Allow one
// reconnect for the download, then suppress further ones to avoid a loop.
int ClientCmdHook(const char* command)
{
    if (IsEnabled() && !control.IsLocal() && SoundcacheControl::IsRetryCommand(command) &&
        !control.TryStartRecovery())
    {
        if (control.TryMarkFailureWarned())
            gEngfuncs.Con_Printf("[SvenSoundcacheControl] Sound list download is still unavailable; waiting without another automatic retry. Check server downloads or set cl_soundcache_alwaysflush 0.\n");
        return 0;
    }
    return originalClientCmd(command);
}

bool __fastcall LoadSoundListHook(void* self, int edx)
{
    if (loadingSoundList)
        return originalLoadSoundList(self, edx);
    // Only observe commands generated by this load, keeping the client's
    // existing command chain and the engine's asynchronous download behavior.
    loadingSoundList             = true;
    originalClientCmd            = clientEngfuncs->pfnClientCmd;
    clientEngfuncs->pfnClientCmd = ClientCmdHook;
    const bool loaded            = originalLoadSoundList(self, edx);
    if (clientEngfuncs->pfnClientCmd == ClientCmdHook)
        clientEngfuncs->pfnClientCmd = originalClientCmd;
    loadingSoundList = false;
    control.FinishLoad(loaded);
    return loaded;
}

void HudInitHook()
{
    alwaysFlush = gEngfuncs.pfnGetCvarPointer(AlwaysFlushCvarName);
    if (!alwaysFlush)
        alwaysFlush = gEngfuncs.pfnRegisterVariable(AlwaysFlushCvarName, "1", FCVAR_CLIENTDLL | FCVAR_ARCHIVE);
    originalHudInit();
}

void HudShutdownHook()
{
    UninstallHooks();
    originalHudShutdown();
}

template <typename Fn>
bool Hook(const char* symbol, void* address, Fn replacement, Fn* original, hook_t** handle)
{
    *handle = g_pMetaHookAPI->InlineHook(address, reinterpret_cast<void*>(replacement), reinterpret_cast<void**>(original));
    if (*handle)
        return true;
    // Roll back earlier hooks before the fatal diagnostic.
    UninstallHooks();
    ReportFailure(symbol, "InlineHook failed");
    return false;
}
} // namespace

bool InstallHooks(cl_exportfuncs_t* exports)
{
    if (mapStartedHook && flushCacheHook && loadSoundListHook)
        return true;
    if (!IsSvenClient())
    {
        g_pMetaHookAPI->SysError("[SvenSoundcacheControl] This plugin requires the Sven Co-op client.");
        return false;
    }
    void* mapStartedAddress       = nullptr;
    void* clearSoundArraysAddress = nullptr;
    void* flushCacheAddress       = nullptr;
    void* loadSoundListAddress    = nullptr;
    void* clientEngfuncsAddress   = nullptr;
    if (!Resolve(MapStartedSymbol, MH_GAMESYMBOL_KIND_FUNCTION, &mapStartedAddress) ||
        !Resolve(ClearSoundArraysSymbol, MH_GAMESYMBOL_KIND_FUNCTION, &clearSoundArraysAddress) ||
        !Resolve(FlushCacheSymbol, MH_GAMESYMBOL_KIND_FUNCTION, &flushCacheAddress) ||
        !Resolve(LoadSoundListSymbol, MH_GAMESYMBOL_KIND_FUNCTION, &loadSoundListAddress) ||
        !Resolve(ClientEngfuncsSymbol, MH_GAMESYMBOL_KIND_GLOBAL, &clientEngfuncsAddress))
        return false;
    clearSoundArrays = reinterpret_cast<ClearSoundArraysFn>(clearSoundArraysAddress);
    clientEngfuncs   = static_cast<cl_enginefunc_t*>(clientEngfuncsAddress);
    if (!Hook(MapStartedSymbol, mapStartedAddress, MapStartedHook, &originalMapStarted, &mapStartedHook) ||
        !Hook(FlushCacheSymbol, flushCacheAddress, FlushCacheHook, &originalFlushCache, &flushCacheHook) ||
        !Hook(LoadSoundListSymbol, loadSoundListAddress, LoadSoundListHook, &originalLoadSoundList, &loadSoundListHook))
        return false;
    originalHudInit       = exports->HUD_Init;
    originalHudShutdown   = exports->HUD_Shutdown;
    exports->HUD_Init     = HudInitHook;
    exports->HUD_Shutdown = HudShutdownHook;
    return true;
}

void UninstallHooks()
{
    for (const auto hook : {&loadSoundListHook, &flushCacheHook, &mapStartedHook})
    {
        if (*hook)
        {
            g_pMetaHookAPI->UnHook(*hook);
            *hook = nullptr;
        }
    }
    originalMapStarted    = nullptr;
    originalFlushCache    = nullptr;
    originalLoadSoundList = nullptr;
    clearSoundArrays      = nullptr;
    clientEngfuncs        = nullptr;
    alwaysFlush           = nullptr;
    control.Reset();
}
} // namespace svensoundcachecontrol
