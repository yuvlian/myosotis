# myosotis-cpp

Native C++ port of the subset of Myosotis BepInEx patches the team needs, built
with `zig cc` (Clang 21) at **C++23** and no external dependencies. Produces a
single `myosotis.dll` plus a `myosotis-loader.exe` suspended-process launcher.

## What it does

Replicates four of the C# Myosotis patches:

- **GuardPatch** — neutralizes the `JsonExtensions` anti-cheat by overwriting
  every method's `MethodInfo->methodPointer` with a return-type-aware stub, and
  stubs `Environment.Exit` / `Application.Quit` / `Environment.FailFast`.
- **Login** — hardcodes the steam token (from `myosotis.ini`) and bypasses the
  browser-login dance: hooks `SteamClient.Init`, `ISteamUser.GetSteamID`,
  `SteamUser.GetAuthSessionTicket`, `PlayerPrefs.GetInt` (account keys → GUEST),
  and the `LoginInfoManager` presence/login methods.
- **Http** — redirects `UnityWebRequest.Post` to the configured server and
  rewrites `notice.limbuscompanyapi.com` + `/serverinfos_*` URLs. CDN redirect
  and the `X-Requested-With` CDN password are dropped.
- **Request** — replaces the game's encrypted transport with plain HTTP via
  WinHTTP: intercepts `HttpApiRequester.AddRequest`, builds a `path → packetId`
  map at init by scanning Assembly-CSharp `*Command` types, POSTs synchronously,
  rewrites `"packetId": N` in the response, and invokes the schema's
  `_responseEvent`.

## How il2cpp names resolve

Limbus ships `GameAssembly.dll` with the ~234 il2cpp C API exports obfuscated
to random 11-char names. The canonical→obfuscated ordering is encoded only in
the order of `LEA [rip+disp32]` instructions inside `LoadIl2CPP` in
`UnityPlayer.dll`. The DLL replicates `analyze_mapping.py` at runtime:

1. Load `UnityPlayer.dll` from disk into a virtual image buffer.
2. Walk `UnityMain → body → impl → LoadIl2CPP` via `E8 rel32` CALL scans.
3. Collect every rip-relative `LEA` whose target is an 11-char ASCII string, in
   instruction order, dedup by first occurrence.
4. Assert the count equals 234 and zip with `IL2CPP_NAMES`.

If the runtime scan ever fails (e.g. UnityPlayer layout shifts), the DLL falls
back to `generated/il2cpp_map.generated.h` — a table precomputed by
`tools/regen_map.py`. Run that script after each Limbus update to keep the
fallback in sync; the runtime scan is the primary path.

Verified on the installed build: runtime scan resolves all 234 names with zero
mismatches vs `analyze_mapping.py`'s output.

## Build

Requires `zig` on PATH (any recent 0.x with Clang 18+; tested with 0.17-dev
at C++23). Builds the DLL, the loader, and the scan self-test:

```bat
cd cpp
build.bat            :: all three targets
build.bat dll        :: just myosotis.dll
build.bat loader     :: just myosotis-loader.exe
build.bat test       :: just test_scan.exe
```

Produces `cpp/build/myosotis.dll`, `cpp/build/myosotis-loader.exe`, and
`cpp/build/test_scan.exe` (the self-test that diffs the runtime scan against
the embedded map; exit 0 on match). All three build with `-O3 -std=c++23`
and the full strict warning set (`-Wall -Wextra -Wpedantic -Wconversion
-Wshadow -Wdouble-promotion -Wformat=2 -Wcast-align -Wnull-dereference
-Wswitch-enum`); zero warnings is the bar.

## Usage

1. Put `myosotis.dll` and `myosotis.ini` next to each other (any directory).
2. Edit `myosotis.ini`:

   ```ini
   [myosotis]
   token=<your hardcoded steam JWT>
   server=https://api.lethelc.site/
   serverinfos_url=https://raw.githubusercontent.com/LEAGUE-OF-NINE/motions-schema/refs/heads/main/serverinfos.json
   log_level=1
   ```

   The first run creates a default `myosotis.ini` with these keys if none exists.
3. Launch the game through the loader:

   ```bat
   myosotis-loader.exe "C:\Program Files (x86)\Steam\steamapps\common\Limbus Company\LimbusCompany.exe" myosotis.dll
   ```

   The loader `CreateProcessW`s the game with `CREATE_SUSPENDED`, injects
   `myosotis.dll` via `CreateRemoteThread(LoadLibraryW, <remote path>)`, waits
   for the load to complete, then `ResumeThread`s the main thread. The DLL's
   init thread polls for `GameAssembly.dll` + `il2cpp_init` done, then installs
   all patches. You can also use any other DLL injector against an already-
   running process — the loader is just a convenience that guarantees the DLL
   is present before the game's first instruction runs.

   Logs go to `OutputDebugStringW` — capture with DebugView or an attached
   debugger.

## Regenerating the embedded fallback map
```bat
python cpp\tools\regen_map.py --game-dir "C:\Program Files (x86)\Steam\steamapps\common\Limbus Company"
```

## Layout

```
cpp/
  include/            hand-written public headers (mirrors src/ layout)
    patches/          patch public interfaces
  generated/         (gitignored) regen_map.py output — embedded fallback map
  src/                DLL sources
    dllmain.cpp       DllMain → init thread
    init.cpp          config → name scan → bridge → patches
    log.cpp           OutputDebugStringW
    config.cpp        myosotis.ini via GetPrivateProfileStringW
    hook.cpp          methodPointer-overwrite hook helper
    http.cpp          synchronous WinHTTP wrapper
    il2cpp/
      pe.cpp           no-dep PE parser
      scan.cpp         byte-pattern CALL/LEA scan
      il2cpp_names.cpp canonical->obfuscated resolver (runtime + fallback)
      il2cpp.cpp       typed il2cpp C API bridge
    patches/
      guard.cpp        anti-cheat neutralization
      login.cpp        hardcoded steam token + SteamClient/SteamUser hooks
      http.cpp         UnityWebRequest redirect + serverinfos rewrite
      request.cpp      AddRequest → synchronous WinHTTP transport
  tools/              dev/diagnostic executables + scripts
    loader.cpp        suspended-process loader (CreateProcess + remote LoadLibrary + resume)
    test_scan.cpp     standalone scan self-test
    regen_map.py      regenerates generated/ from UnityPlayer.dll
    analyze_mapping.py copied from myosotis/la-ng — the offline resolver regen_map.py drives
    server.py         logging HTTP server for local dev (127.0.0.1:3000)
  build.bat           build driver — `build.bat [dll|loader|test|all]`
  .gitignore          ignores build/ and generated/

## Known limitations / TODO

- **Request patch `apiPath` resolution.** The C# version calls the schema's
  `get_ApiClass` (returns an enum) and lowercases `enum.ToString()`. We resolve
  the enum value's integer but haven't wired up `System.Enum.GetName` to recover
  the member name — so the `path → packetId` map keys currently use the string
  part of the API path only. This needs `System.Enum.GetName(Type, object)` and
  `Type.GetTypeFromHandle` wired through the il2cpp bridge to be fully faithful.
  See the `build_api_path` function in `src/patches/request.cpp`.
- **AuthTicket / SteamId field offsets** are hardcoded (0x10 after the object
  header) rather than resolved via `il2cpp_class_get_field_from_name`. Robust
  against the current build; would need the field-by-name path if Steamworks.NET
  layout changes.
- **`_responseEvent` field offset** on `HttpApiSchema` is hardcoded to 0x18.
  Same caveat — should be resolved by field name for long-term stability.
