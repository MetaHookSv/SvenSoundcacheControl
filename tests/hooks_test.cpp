// Exercise the production hooks with an asynchronous-download model of the SDK.
#include "privatehook.h"
#include "soundcache_control.h"
#include <cvardef.h>
#include <demo_api.h>
#include <net_api.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <vector>

metahook_api_t* g_pMetaHookAPI = nullptr;
cl_enginefunc_t gEngfuncs      = {};

namespace
{
using svensoundcachecontrol::SoundcacheControl;

constexpr uintptr_t   ClientBaseAddress = 0x10000000;
constexpr int         HookCount         = 3;
constexpr const char* FunctionSymbols[] = {
    "CClient_SoundEngine_MapStarted",
    "CClient_SoundEngine_ClearSoundArrays",
    "CClient_SoundEngine_FlushCache",
    "CClient_SoundEngine_LoadSoundList",
};
constexpr const char* ClientEngfuncsSymbol = "gEngfuncs";

int                    failures      = 0;
int                    installs      = 0;
int                    failInstall   = 0;
bool                   svenClient    = true;
const char*            missingSymbol = nullptr;
char                   error[1024]   = {};
std::vector<uintptr_t> removed;
cl_enginefunc_t        clientEngfuncs = {};
cl_exportfuncs_t       exports        = {};
cvar_t                 alwaysFlush    = {};
const char*            registeredCvar = nullptr;

// SDK model: the disk list, the in-memory indices and the current connection.
bool diskList     = true;
bool soundsLoaded = false;
bool loopback     = false;
bool demoPlayback = false;
int  clears       = 0;
int  mapStarts    = 0;
int  fullFlushes  = 0;
int  retries      = 0;
int  warnings     = 0;
int  hudInits     = 0;
int  hudShutdowns = 0;

svensoundcachecontrol::MapStartedFn    hookedMapStarted    = nullptr;
svensoundcachecontrol::FlushCacheFn    hookedFlushCache    = nullptr;
svensoundcachecontrol::LoadSoundListFn hookedLoadSoundList = nullptr;

void Check(const char* label, int expected, int actual)
{
    if (expected != actual)
    {
        std::printf("FAIL %s: expected %d, got %d\n", label, expected, actual);
        ++failures;
    }
}

const char* Level() { return "maps/test.bsp"; }
char*       Address(netadr_t*) { return const_cast<char*>(loopback ? "loopback" : "192.0.2.1:27015"); }
int         Playback() { return demoPlayback; }
void        Status(net_status_t* status)
{
    status->connected           = 1;
    status->remote_address.type = loopback ? NA_LOOPBACK : NA_IP;
}
void    Printf(const char*, ...) { ++warnings; }
cvar_t* GetCvarPointer(const char*) { return nullptr; }
cvar_t* RegisterVariable(const char* name, const char*, int)
{
    registeredCvar = name;
    return &alwaysFlush;
}
int ClientCmd(const char* command)
{
    if (SoundcacheControl::IsRetryCommand(command))
        ++retries;
    return 1;
}
void HudInit() { ++hudInits; }
void HudShutdown() { ++hudShutdowns; }

void __fastcall OriginalMapStarted(void*, int) { ++mapStarts; }
void __fastcall OriginalClearSoundArrays(void*, int)
{
    ++clears;
    soundsLoaded = false;
}
void __fastcall OriginalFlushCache(void*, int, bool includingLocal)
{
    if (includingLocal)
    {
        ++fullFlushes;
        diskList = false;
    }
}
// The original loader queues the download, then reconnects when the list is missing.
bool __fastcall OriginalLoadSoundList(void* self, int edx)
{
    clientEngfuncs.pfnClientCmd("dlfile maps/soundcache/test.txt\n");
    if (!diskList)
    {
        clientEngfuncs.pfnClientCmd("retry");
        return false;
    }
    soundsLoaded = true;
    hookedFlushCache(self, edx, false);
    return true;
}

IBaseInterface* ClientFactory(const char* name, int*)
{
    return svenClient && std::strcmp(name, "SCClientDLL001") == 0 ? reinterpret_cast<IBaseInterface*>(&exports) : nullptr;
}
CreateInterfaceFn      GetClientFactory() { return ClientFactory; }
PVOID                  ClientBase() { return reinterpret_cast<void*>(ClientBaseAddress); }
DWORD                  EngineBuild() { return 10257; }
mh_gamesymbol_status_t CRC64(PVOID, uint64_t* value)
{
    *value = 0x1234;
    return MH_GAMESYMBOL_OK;
}
const char* StatusString(mh_gamesymbol_status_t) { return "SYMBOL_NOT_FOUND"; }
void        SysError(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::vsnprintf(error, sizeof(error), format, args);
    va_end(args);
}
mh_gamesymbol_status_t Resolve(PVOID base, const char* symbol, mh_gamesymbol_kind_t kind, PVOID* address)
{
    Check("resolve real client base", 1, base == ClientBase());
    if (missingSymbol && std::strcmp(symbol, missingSymbol) == 0)
        return MH_GAMESYMBOL_SYMBOL_NOT_FOUND;
    if (std::strcmp(symbol, ClientEngfuncsSymbol) == 0)
    {
        Check("client engine table is a global", MH_GAMESYMBOL_KIND_GLOBAL, kind);
        *address = &clientEngfuncs;
        return MH_GAMESYMBOL_OK;
    }
    Check("sound engine members are functions", MH_GAMESYMBOL_KIND_FUNCTION, kind);
    if (std::strcmp(symbol, FunctionSymbols[0]) == 0)
        *address = reinterpret_cast<void*>(OriginalMapStarted);
    else if (std::strcmp(symbol, FunctionSymbols[1]) == 0)
        *address = reinterpret_cast<void*>(OriginalClearSoundArrays);
    else if (std::strcmp(symbol, FunctionSymbols[2]) == 0)
        *address = reinterpret_cast<void*>(OriginalFlushCache);
    else
        *address = reinterpret_cast<void*>(OriginalLoadSoundList);
    return MH_GAMESYMBOL_OK;
}
hook_t* InlineHook(void* address, void* replacement, void** original)
{
    ++installs;
    if (failInstall == installs)
        return nullptr;
    *original = address;
    if (address == reinterpret_cast<void*>(OriginalMapStarted))
        hookedMapStarted = reinterpret_cast<svensoundcachecontrol::MapStartedFn>(replacement);
    else if (address == reinterpret_cast<void*>(OriginalFlushCache))
        hookedFlushCache = reinterpret_cast<svensoundcachecontrol::FlushCacheFn>(replacement);
    else
        hookedLoadSoundList = reinterpret_cast<svensoundcachecontrol::LoadSoundListFn>(replacement);
    return reinterpret_cast<hook_t*>(static_cast<uintptr_t>(installs));
}
BOOL UnHook(hook_t* handle)
{
    removed.push_back(reinterpret_cast<uintptr_t>(handle));
    return TRUE;
}

void Reset()
{
    svensoundcachecontrol::UninstallHooks();
    installs = failInstall = 0;
    svenClient             = true;
    missingSymbol          = nullptr;
    error[0]               = '\0';
    removed.clear();
    clientEngfuncs              = {};
    clientEngfuncs.pfnClientCmd = ClientCmd;
    exports                     = {};
    exports.HUD_Init            = HudInit;
    exports.HUD_Shutdown        = HudShutdown;
    alwaysFlush                 = {};
    alwaysFlush.value           = 1;
    registeredCvar              = nullptr;
    hookedMapStarted            = nullptr;
    hookedFlushCache            = nullptr;
    hookedLoadSoundList         = nullptr;
}

void Install()
{
    Reset();
    Check("install succeeds", 1, svensoundcachecontrol::InstallHooks(&exports));
    Check("install hooks the sound engine", HookCount, installs);
    Check("repeated install succeeds", 1, svensoundcachecontrol::InstallHooks(&exports));
    Check("repeated install adds no hooks", HookCount, installs);
    exports.HUD_Init();
    Check("HUD_Init registers the cvar", 1, registeredCvar && std::strcmp(registeredCvar, "cl_soundcache_alwaysflush") == 0);
    Check("HUD_Init calls the original", 1, hudInits);
}

void Recovery()
{
    Install();
    diskList = true;
    hookedMapStarted(nullptr, 0);
    Check("join invalidates stale disk list", 0, diskList);
    Check("join resets memory", 1, clears);
    Check("join preserves original map callback", 1, mapStarts);
    Check("missing list is not reported as loaded", 0, hookedLoadSoundList(nullptr, 0));
    Check("original loader gets one recovery retry", 1, retries);
    Check("client command chain restored after failed load", 1, clientEngfuncs.pfnClientCmd == ClientCmd);

    diskList = true; // Simulate completion of the queued download.
    hookedFlushCache(nullptr, 0, false);
    Check("disconnect does not delete completed recovery download", 1, diskList);
    hookedMapStarted(nullptr, 0);
    Check("recovery join preserves downloaded file", 1, diskList);
    Check("recovery consumes the new list", 1, hookedLoadSoundList(nullptr, 0));
    Check("sound indices are populated", 1, soundsLoaded);
    Check("loader final flush invalidates the consumed list", 0, diskList);
    Check("command chain restored after successful load", 1, clientEngfuncs.pfnClientCmd == ClientCmd);

    diskList = true; // A queued request can recreate the file after loading.
    hookedMapStarted(nullptr, 0);
    Check("subsequent same-map join invalidates asynchronously recreated list", 0, diskList);
    Check("same-map join discards previous indices", 0, soundsLoaded);
    hookedLoadSoundList(nullptr, 0);
    hookedMapStarted(nullptr, 0);
    hookedLoadSoundList(nullptr, 0); // Download remains unavailable on recovery join.
    hookedLoadSoundList(nullptr, 0);
    Check("failed transfer does not produce a reconnect loop", 2, retries);
    Check("transfer failure is reported once", 1, warnings);
    Check("command chain restored after exhausted retry", 1, clientEngfuncs.pfnClientCmd == ClientCmd);
}

void VanillaModes()
{
    alwaysFlush.value              = 0;
    const int clearsBeforeDisable  = clears;
    const int flushesBeforeDisable = fullFlushes;
    diskList                       = true;
    hookedMapStarted(nullptr, 0);
    hookedLoadSoundList(nullptr, 0);
    Check("disabled mode retains vanilla disk behavior", 1, diskList);
    Check("disabled mode does not force a memory reset", clearsBeforeDisable, clears);
    Check("disabled mode does not force a full flush", flushesBeforeDisable, fullFlushes);

    alwaysFlush.value = 1;
    loopback          = true;
    diskList          = true;
    hookedMapStarted(nullptr, 0);
    Check("listen host loads generated sound list", 1, hookedLoadSoundList(nullptr, 0));
    Check("listen host leaves server download source intact", 1, diskList);

    demoPlayback               = true;
    const int clearsBeforeDemo = clears;
    hookedMapStarted(nullptr, 0);
    Check("demo playback uses original memory behavior", clearsBeforeDemo, clears);
    loopback     = false;
    demoPlayback = false;
}

void Shutdown()
{
    exports.HUD_Shutdown();
    Check("client shutdown removes all hooks", HookCount, static_cast<int>(removed.size()));
    Check("client shutdown calls the original", 1, hudShutdowns);
    svensoundcachecontrol::UninstallHooks();
    Check("later host teardown cannot unhook stale handles twice", HookCount, static_cast<int>(removed.size()));
}

void Failures()
{
    Reset();
    svenClient = false;
    Check("other clients are rejected", 0, svensoundcachecontrol::InstallHooks(&exports));
    Check("other clients install nothing", 0, installs);
    Check("other clients get a diagnostic", 1, std::strstr(error, "Sven Co-op client") != nullptr);

    std::vector<const char*> symbols(std::begin(FunctionSymbols), std::end(FunctionSymbols));
    symbols.push_back(ClientEngfuncsSymbol);
    for (const auto symbol : symbols)
    {
        Reset();
        missingSymbol = symbol;
        Check("missing symbol fails", 0, svensoundcachecontrol::InstallHooks(&exports));
        Check("missing symbol installs nothing", 0, installs);
        Check("missing symbol keeps exports", 1, exports.HUD_Init == HudInit && exports.HUD_Shutdown == HudShutdown);
        Check("error names symbol", 1, std::strstr(error, symbol) != nullptr);
        Check("error contains CRC64", 1, std::strstr(error, "0000000000001234") != nullptr);
        Check("error contains build", 1, std::strstr(error, "10257") != nullptr);
        Check("error contains status", 1, std::strstr(error, "SYMBOL_NOT_FOUND") != nullptr);
    }
    for (int failed = 1; failed <= HookCount; ++failed)
    {
        Reset();
        failInstall = failed;
        Check("hook failure fails installation", 0, svensoundcachecontrol::InstallHooks(&exports));
        Check("hook failure rolls back prior hooks", failed - 1, static_cast<int>(removed.size()));
        Check("hook failure keeps exports", 1, exports.HUD_Init == HudInit && exports.HUD_Shutdown == HudShutdown);
        svensoundcachecontrol::UninstallHooks();
        Check("rollback leaves no hooks", failed - 1, static_cast<int>(removed.size()));
        Check("hook failure diagnostic", 1, std::strstr(error, "InlineHook failed") != nullptr);
    }
    Reset();
    Check("install can retry after failure", 1, svensoundcachecontrol::InstallHooks(&exports));
    svensoundcachecontrol::UninstallHooks();
}
} // namespace

int main()
{
    metahook_api_t api            = {};
    api.GetClientBase             = ClientBase;
    api.GetClientFactory          = GetClientFactory;
    api.GetEngineBuildnum         = EngineBuild;
    api.GetModuleCRC64            = CRC64;
    api.GetGameSymbolStatusString = StatusString;
    api.SysError                  = SysError;
    api.ResolveGameSymbol         = Resolve;
    api.InlineHook                = InlineHook;
    api.UnHook                    = UnHook;
    g_pMetaHookAPI                = &api;
    net_api_t net                 = {};
    net.Status                    = Status;
    net.AdrToString               = Address;
    demo_api_t demo               = {};
    demo.IsPlayingback            = Playback;
    gEngfuncs.pNetAPI             = &net;
    gEngfuncs.pDemoAPI            = &demo;
    gEngfuncs.pfnGetLevelName     = Level;
    gEngfuncs.Con_Printf          = Printf;
    gEngfuncs.pfnGetCvarPointer   = GetCvarPointer;
    gEngfuncs.pfnRegisterVariable = RegisterVariable;
    Recovery();
    VanillaModes();
    Shutdown();
    Failures();
    std::printf("SvenSoundcacheControl hooks: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
