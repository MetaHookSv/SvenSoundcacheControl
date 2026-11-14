// Exercise the production hooks with an asynchronous-download model of the SDK.
#include "privatehook.h"
#include "soundcache_control.h"
#include <cvardef.h>
#include <demo_api.h>
#include <net_api_svengine.h>
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

// Byte offsets observed in both Sven engine builds, independent of SDK types.
constexpr std::size_t NativeAddressSize         = 36;
constexpr std::size_t NativeStatusSize          = 104;
constexpr std::size_t NativeLocalAddressOffset  = 4;
constexpr std::size_t NativeRemoteAddressOffset = 40;
constexpr std::size_t NativePacketLossOffset    = 76;
constexpr std::size_t NativeLatencyOffset       = 80;
constexpr std::size_t NativeConnectionOffset    = 88;
constexpr std::size_t NativeRateOffset          = 96;

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
int  hudVidInits  = 0;
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
void        NativeAddress(unsigned char* bytes, bool local)
{
    std::memset(bytes, 0xA5, NativeAddressSize);
    const int type = local ? NA_LOOPBACK : NA_IP;
    std::memcpy(bytes, &type, sizeof(type));
}
char* Address(netadr_svengine_t* address)
{
    unsigned char expected[NativeAddressSize];
    NativeAddress(expected, loopback);
    Check("address conversion receives the complete native remote address", 0, std::memcmp(expected, address, sizeof(expected)));
    return const_cast<char*>(loopback ? "loopback" : "192.0.2.1:27015");
}
int  Playback() { return demoPlayback; }
void Status(net_status_svengine_t* status)
{
    unsigned char bytes[NativeStatusSize] = {};
    const int     connected               = 1;
    const int     packetLoss              = 7;
    const double  latency                 = 0.125;
    const double  connectionTime          = 12.5;
    const double  rate                    = 20000.0;
    std::memcpy(bytes, &connected, sizeof(connected));
    NativeAddress(bytes + NativeLocalAddressOffset, true);
    NativeAddress(bytes + NativeRemoteAddressOffset, loopback);
    std::memcpy(bytes + NativePacketLossOffset, &packetLoss, sizeof(packetLoss));
    std::memcpy(bytes + NativeLatencyOffset, &latency, sizeof(latency));
    std::memcpy(bytes + NativeConnectionOffset, &connectionTime, sizeof(connectionTime));
    std::memcpy(bytes + NativeRateOffset, &rate, sizeof(rate));
    // Write the complete native layout, rather than mirroring SDK field access.
    std::memcpy(status, bytes, sizeof(bytes));
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
int  HudVidInit()
{
    ++hudVidInits;
    return 1;
}
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
    exports.HUD_VidInit         = HudVidInit;
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
    const int initsBefore = hudInits;
    Reset();
    Check("install succeeds", 1, svensoundcachecontrol::InstallHooks(&exports));
    Check("install hooks the sound engine", HookCount, installs);
    Check("repeated install succeeds", 1, svensoundcachecontrol::InstallHooks(&exports));
    Check("repeated install adds no hooks", HookCount, installs);
    exports.HUD_Init();
    Check("HUD_Init registers the cvar", 1, registeredCvar && std::strcmp(registeredCvar, "cl_soundcache_alwaysflush") == 0);
    Check("HUD_Init calls the original", initsBefore + 1, hudInits);
}

void NetworkABI()
{
    constexpr uint64_t Guard = 0x0123456789abcdef;
    struct GuardedStatus
    {
        uint64_t              before;
        net_status_svengine_t status;
        uint64_t              after;
    } guarded = {Guard, {}, Guard};
    Status(&guarded.status);
    Check("native status write preserves leading guard", 1, guarded.before == Guard);
    Check("native status write preserves trailing guard", 1, guarded.after == Guard);
    Check("legacy network address ABI is unchanged", 20, sizeof(netadr_t));
    Check("legacy network status ABI is unchanged", 72, sizeof(net_status_t));
    Check("native connected field", 1, guarded.status.connected);
    Check("native local address field", NA_LOOPBACK, guarded.status.local_address.type);
    Check("native remote address field", NA_IP, guarded.status.remote_address.type);
    Check("native packet loss field", 7, guarded.status.packet_loss);
    Check("native latency field", 1, guarded.status.latency == 0.125);
    Check("native connection time field", 1, guarded.status.connection_time == 12.5);
    Check("native rate field", 1, guarded.status.rate == 20000.0);
    Address(&guarded.status.remote_address);
}

void StartMap()
{
    const int vidInitsBefore = hudVidInits;
    Check("HUD_VidInit preserves the original result", 1, exports.HUD_VidInit());
    Check("HUD_VidInit calls the original", vidInitsBefore + 1, hudVidInits);
    hookedMapStarted(nullptr, 0);
}

void EarlySoundLoad()
{
    Install();
    const int retriesBefore = retries;
    diskList                = true;
    soundsLoaded            = true;
    exports.HUD_VidInit();
    Check("early missing list requests recovery", 0, hookedLoadSoundList(nullptr, 0));
    Check("early load invalidates the previous disk list", 0, diskList);
    Check("early load discards previous sound indices", 0, soundsLoaded);
    Check("early load permits one recovery retry", retriesBefore + 1, retries);

    diskList = true;
    exports.HUD_VidInit();
    Check("recovery loads sounds before the map callback", 1, hookedLoadSoundList(nullptr, 0));
    const int clearsBeforeCallback  = clears;
    const int flushesBeforeCallback = fullFlushes;
    hookedMapStarted(nullptr, 0);
    Check("late map callback preserves freshly loaded sounds", 1, soundsLoaded);
    Check("late map callback does not clear sounds again", clearsBeforeCallback, clears);
    Check("late map callback does not invalidate the list again", flushesBeforeCallback, fullFlushes);
    Check("late map callback cannot restart recovery", retriesBefore + 1, retries);

    diskList = true;
    StartMap();
    Check("next same-map join still refreshes memory", 0, soundsLoaded);
    Check("next same-map join still invalidates its list", 0, diskList);
}

void Recovery()
{
    Install();
    diskList = true;
    StartMap();
    Check("join invalidates stale disk list", 0, diskList);
    Check("join resets memory", 1, clears);
    Check("join preserves original map callback", 1, mapStarts);
    Check("missing list is not reported as loaded", 0, hookedLoadSoundList(nullptr, 0));
    Check("original loader gets one recovery retry", 1, retries);
    Check("client command chain restored after failed load", 1, clientEngfuncs.pfnClientCmd == ClientCmd);

    diskList = true; // Simulate completion of the queued download.
    hookedFlushCache(nullptr, 0, false);
    Check("disconnect does not delete completed recovery download", 1, diskList);
    StartMap();
    Check("recovery join preserves downloaded file", 1, diskList);
    Check("recovery consumes the new list", 1, hookedLoadSoundList(nullptr, 0));
    Check("sound indices are populated", 1, soundsLoaded);
    Check("loader final flush invalidates the consumed list", 0, diskList);
    Check("command chain restored after successful load", 1, clientEngfuncs.pfnClientCmd == ClientCmd);

    diskList = true; // A queued request can recreate the file after loading.
    StartMap();
    Check("subsequent same-map join invalidates asynchronously recreated list", 0, diskList);
    Check("same-map join discards previous indices", 0, soundsLoaded);
    hookedLoadSoundList(nullptr, 0);
    StartMap();
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
    StartMap();
    hookedLoadSoundList(nullptr, 0);
    Check("disabled mode retains vanilla disk behavior", 1, diskList);
    Check("disabled mode does not force a memory reset", clearsBeforeDisable, clears);
    Check("disabled mode does not force a full flush", flushesBeforeDisable, fullFlushes);

    alwaysFlush.value = 1;
    loopback          = true;
    diskList          = true;
    StartMap();
    Check("listen host loads generated sound list", 1, hookedLoadSoundList(nullptr, 0));
    Check("listen host leaves server download source intact", 1, diskList);

    demoPlayback               = true;
    const int clearsBeforeDemo = clears;
    StartMap();
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
        Check("missing symbol keeps exports", 1, exports.HUD_Init == HudInit && exports.HUD_VidInit == HudVidInit && exports.HUD_Shutdown == HudShutdown);
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
        Check("hook failure keeps exports", 1, exports.HUD_Init == HudInit && exports.HUD_VidInit == HudVidInit && exports.HUD_Shutdown == HudShutdown);
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
    net.Status                    = reinterpret_cast<decltype(net.Status)>(Status);
    net.AdrToString               = reinterpret_cast<decltype(net.AdrToString)>(Address);
    demo_api_t demo               = {};
    demo.IsPlayingback            = Playback;
    gEngfuncs.pNetAPI             = &net;
    gEngfuncs.pDemoAPI            = &demo;
    gEngfuncs.pfnGetLevelName     = Level;
    gEngfuncs.Con_Printf          = Printf;
    gEngfuncs.pfnGetCvarPointer   = GetCvarPointer;
    gEngfuncs.pfnRegisterVariable = RegisterVariable;
    NetworkABI();
    Recovery();
    VanillaModes();
    Shutdown();
    EarlySoundLoad();
    Failures();
    std::printf("SvenSoundcacheControl hooks: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
