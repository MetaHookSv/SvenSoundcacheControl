// Exercise the production hooks with an asynchronous-download model of the SDK.
#include "../src/plugins.cpp"
#include <cstdio>
#include <cstdlib>

namespace
{
bool testFile        = true;
bool testLoaded      = false;
bool testLoopback    = false;
bool testDemo        = false;
int  testClears      = 0;
int  testMapStarts   = 0;
int  testFullFlushes = 0;
int  testRetries     = 0;
int  testWarnings    = 0;
int  testUnhooks     = 0;
int  testShutdowns   = 0;

BOOL Unhook(hook_t*)
{
    ++testUnhooks;
    return TRUE;
}
void OriginalShutdown() { ++testShutdowns; }

void Check(bool expected, bool actual, const char* scenario)
{
    if (expected != actual)
    {
        std::fprintf(stderr, "FAILED: %s\n", scenario);
        std::exit(EXIT_FAILURE);
    }
}

const char* Level() { return "maps/test.bsp"; }
char*       Address(netadr_t*) { return const_cast<char*>(testLoopback ? "loopback" : "192.0.2.1:27015"); }
int         Playback() { return testDemo; }
void        Status(net_status_t* status)
{
    status->connected           = 1;
    status->remote_address.type = testLoopback ? NA_LOOPBACK : NA_IP;
}
void Printf(const char*, ...) { ++testWarnings; }
int  Command(const char* command)
{
    if (SoundcacheControl::IsRetryCommand(command))
        ++testRetries;
    return 1;
}
void __fastcall OriginalMapStarted(void*, int) { ++testMapStarts; }
void __fastcall OriginalClear(void*, int)
{
    ++testClears;
    testLoaded = false;
}
void __fastcall OriginalFlush(void*, int, bool all)
{
    if (all)
    {
        ++testFullFlushes;
        testFile = false;
    }
}
bool __fastcall OriginalLoad(void* engine, int dummy)
{
    g_ClientEngine->pfnClientCmd("dlfile maps/soundcache/test.txt\n");
    if (!testFile)
    {
        g_ClientEngine->pfnClientCmd("retry");
        return false;
    }
    testLoaded = true;
    FlushCache(engine, dummy, false);
    return true;
}
} // namespace

int main()
{
    cvar_t enabled           = {};
    enabled.value            = 1;
    g_AlwaysFlush            = &enabled;
    net_api_t net            = {};
    net.Status               = Status;
    net.AdrToString          = Address;
    demo_api_t demo          = {};
    demo.IsPlayingback       = Playback;
    g_Engine.pNetAPI         = &net;
    g_Engine.pDemoAPI        = &demo;
    g_Engine.pfnGetLevelName = Level;
    g_Engine.Con_Printf      = Printf;
    cl_enginefunc_t client   = {};
    client.pfnClientCmd      = Command;
    g_ClientEngine           = &client;
    g_MapStarted             = OriginalMapStarted;
    g_ClearSoundArrays       = OriginalClear;
    g_FlushCache             = OriginalFlush;
    g_LoadSoundList          = OriginalLoad;

    MapStarted(nullptr, 0);
    Check(false, testFile, "join invalidates stale disk list");
    Check(true, 1 == testClears && 1 == testMapStarts, "join resets memory and preserves original map callback");
    Check(false, LoadSoundList(nullptr, 0), "missing list is not reported as loaded");
    Check(true, 1 == testRetries, "original loader gets one recovery retry");
    Check(true, Command == client.pfnClientCmd, "client command chain restored after failed load");

    testFile = true; // Simulate completion of the queued download.
    FlushCache(nullptr, 0, false);
    Check(true, testFile, "disconnect does not delete completed recovery download");
    MapStarted(nullptr, 0);
    Check(true, testFile, "recovery join preserves downloaded file");
    Check(true, LoadSoundList(nullptr, 0), "recovery consumes the new list");
    Check(true, testLoaded, "sound indices are populated");
    Check(true, Command == client.pfnClientCmd, "command chain restored after successful load");

    testFile = true; // A queued request can recreate the file after loading.
    MapStarted(nullptr, 0);
    Check(false, testFile, "subsequent same-map join invalidates asynchronously recreated list");
    Check(false, testLoaded, "same-map join discards previous indices");
    LoadSoundList(nullptr, 0);
    MapStarted(nullptr, 0);
    LoadSoundList(nullptr, 0); // Download remains unavailable on recovery join.
    LoadSoundList(nullptr, 0);
    Check(true, 2 == testRetries, "failed transfer does not produce a reconnect loop");
    Check(true, 1 == testWarnings, "transfer failure is reported once");
    Check(true, Command == client.pfnClientCmd, "command chain restored after exhausted retry");

    enabled.value                  = 0;
    const int clearsBeforeDisable  = testClears;
    const int flushesBeforeDisable = testFullFlushes;
    testFile                       = true;
    MapStarted(nullptr, 0);
    LoadSoundList(nullptr, 0);
    Check(true, testFile, "disabled mode retains vanilla disk behavior");
    Check(true, clearsBeforeDisable == testClears && flushesBeforeDisable == testFullFlushes,
          "disabled mode does not force a memory reset or full flush");

    enabled.value = 1;
    testLoopback  = true;
    testFile      = true;
    MapStarted(nullptr, 0);
    Check(true, LoadSoundList(nullptr, 0), "listen host loads generated sound list");
    Check(true, testFile, "listen host leaves server download source intact");

    testDemo                   = true;
    const int clearsBeforeDemo = testClears;
    MapStarted(nullptr, 0);
    Check(true, clearsBeforeDemo == testClears, "demo playback uses original memory behavior");
    metahook_api_t api     = {};
    api.UnHook             = Unhook;
    g_API                  = &api;
    g_MapHook              = reinterpret_cast<hook_t*>(&testMapStarts);
    g_FlushHook            = reinterpret_cast<hook_t*>(&testFullFlushes);
    g_LoadHook             = reinterpret_cast<hook_t*>(&testLoaded);
    g_Exports.HUD_Shutdown = OriginalShutdown;
    HUD_Shutdown();
    Check(true, 3 == testUnhooks && 1 == testShutdowns, "client shutdown removes all hooks before calling original");
    IPluginsV4 plugin;
    plugin.ExitGame(0);
    plugin.Shutdown();
    Check(true, 3 == testUnhooks, "later host teardown cannot unhook stale handles twice");
    std::puts("Production soundcache hook scenarios passed.");
    return EXIT_SUCCESS;
}
