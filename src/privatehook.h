#pragma once

#include <metahook.h>

namespace svensoundcachecontrol
{
constexpr int RequiredMetaHookAPIVersion = 109;

// Native CClient_SoundEngine members are __thiscall; __fastcall with an unused
// EDX slot receives `this` in ECX and keeps the callee-cleaned stack layout.
using MapStartedFn       = void(__fastcall*)(void* self, int edx);
using ClearSoundArraysFn = void(__fastcall*)(void* self, int edx);
using FlushCacheFn       = void(__fastcall*)(void* self, int edx, bool includingLocal);
using LoadSoundListFn    = bool(__fastcall*)(void* self, int edx);

// Hooks the client sound engine and wraps HUD_Init/HUD_VidInit/HUD_Shutdown.
bool InstallHooks(cl_exportfuncs_t* exports);
void UninstallHooks();
} // namespace svensoundcachecontrol
