# SvenSoundcacheControl

A MetaHookSv client plugin for Sven Co-op. Refreshes sound lists when joining a
server, including reconnecting to the same map after the server changes its sound
precache order. Addresses come from the published GoldSrc_VibeSignatures catalog
(soundcache entry points added in v20261113a).

## Install and configure

Copy `SvenSoundcacheControl.dll` and `gamedata/svensoundcachecontrol` from the
package into `svencoop/metahook/` (the DLL belongs in `plugins/`). Add
`SvenSoundcacheControl.dll` to `metahook/configs/plugins.lst` to enable it. The
aggregate distributes this plugin without adding it to the default plugin lists.

| Client cvar | Default | Behavior |
| --- | --- | --- |
| `cl_soundcache_alwaysflush` | `1` | Refresh sound lists on every join. `0` restores the original client behavior. Saved in the client configuration. |

Changes apply to subsequent cache operations and map starts; toggling the cvar
does not interrupt currently playing audio or reconnect immediately. Disabling
cannot restore a disk list already removed. The old DontFlushSoundCache mode is
not included; do not load both plugins.

## Behavior and limits

The original sound engine clears its in-memory indices only when the map name
changes. This plugin calls its native `ClearSoundArrays` on every map start and
invalidates the native disk sound-list cache, then uses the original loader.
It does not delete audio assets or implement a replacement sound engine.

A remote join can require one automatic reconnect while the original `dlfile`
request completes. The recovery join preserves downloaded files. If downloading
still fails, additional automatic reconnects are suppressed and the original
loader keeps waiting on its normal cooldown; a diagnostic explains the failure.
Server downloads must be available. Download failure is not reported as success.
Demo playback keeps the original cache behavior.

For a listen server's local loopback client, memory indices are rebuilt from the
server-generated list using original disk cleanup; full disk invalidation would
remove the file the same server must serve to other players.

Windows MSVC x86 only, MetaHook API 109 or newer, Sven client builds 8948/10257.
Other clients and missing gamedata produce a clear startup error. Catalog
coverage alone does not imply that every build has been tested in a live game.

## Build and test

Requires Visual Studio 2022, CMake 3.21+, Python 3.9+, and network access to the
symbol catalog. The MetaHook source tree supplies SDK headers, `interface.cpp`
and the shared gamedata synchronization/validation scripts. It is fetched from
`main` unless `METAHOOK_SOURCE_PATH` is set; no SDK library is linked.

```powershell
cmake -S . -B build/x86/Release -G "Visual Studio 17 2022" -A Win32 -DMETAHOOK_SOURCE_PATH=../../MetaHook -DCMAKE_INSTALL_PREFIX=install/x86/Release
cmake --build build/x86/Release --config Release --target install
ctest --test-dir build/x86/Release -C Release --output-on-failure
```

Use the same commands with `Debug` for a debug build. The release wrapper is
`scripts/build-SvenSoundcacheControl-x86-Release.bat`. Both builds use the static
MSVC CRT and retain the same DLL name. CI validates packaged gamedata and runs
the behavior tests before packaging.

The aggregate injects local SDK and shared FormatValidation sources. Standalone
builds fetch their dependencies without nested submodules. Formatting uses the
shared clang-format 23.1.3 configuration (`format`, `format-check` targets).
`FORMAT_VALIDATION_ONLY=ON` configures formatting without a native compiler.

Optional Visual Studio F5 deployment uses `METAHOOKSV_ENABLE_LAUNCH_GAME=ON` and
the shared LaunchGame module; it requires an existing MetaHook installation with
this plugin already enabled. Normal builds do not deploy to a game.

Issue: https://github.com/MetaHookSv/MetaHookSv/issues/974
