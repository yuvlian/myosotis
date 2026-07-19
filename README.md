# myosotis

Native C++ port of the subset of the Lethe BepInEx patches the team needs, built
with `zig cc` (Clang 21) at **C++23** and no external dependencies. Produces a
single injectable `myosotis.dll` and a `myoink.exe` loader.

## What it does

Replicates four of the C# patches:

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

## How method hooking works

il2cpp's managed-to-managed calls use **baked-in direct native call addresses**
— the compiled caller jumps straight to the method's native body, never reading
`MethodInfo->methodPointer`. Overwriting `methodPointer` alone only intercepts
`runtime_invoke`-based calls (reflection), not normal managed calls.

`hook::install_inline` (in `src/hook.cpp`) solves this with **inline hooking**:
it patches the native code body at `methodPointer` with a jump to our stub, so
both direct managed calls and `runtime_invoke` paths are intercepted. It also
overwrites `methodPointer` for completeness.

Because some hooks need to call the original after running (e.g. `SendWebRequest`
returns a coroutine the caller needs, `Post` must actually send the request),
`install_inline` builds a **trampoline**: a small x86-64 instruction length
decoder walks the original prologue, copies enough instructions (≥ the patch
size) to a freshly `VirtualAlloc`'d executable page, then appends a jump back to
`original+N`. The trampoline runs the displaced prologue then resumes the
original, so prefix hooks can call through to it.

Two patch sizes are used:

- **5-byte relative jump** (`E9 rel32`) when the stub is within ±2GB (the common
  case — our DLL loads near the game).
- **12-byte absolute jump** (`mov rax, imm64; jmp rax`) otherwise.

If the length decoder can't safely cover the patch size (unknown instruction
before the boundary), the inline patch is skipped and the method falls back to
`methodPointer`-only — safe but may miss direct calls for that one method.

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
build.bat loader     :: just myoink.exe
build.bat test       :: just test_scan.exe
```

Produces `cpp/build/myosotis.dll`, `cpp/build/myoink.exe`, and
`cpp/build/test_scan.exe` (the self-test that diffs the runtime scan against
the embedded map; exit 0 on match). All three build with `-O3 -std=c++23`
and the full strict warning set (`-Wall -Wextra -Wpedantic -Wconversion
-Wshadow -Wdouble-promotion -Wformat=2 -Wcast-align -Wnull-dereference
-Wswitch-enum`); zero warnings is the bar.

## Usage

1. Put `myosotis.dll`, `myosotis.ini`, and `myoink.exe` next to each other
   (any directory — the loader finds the DLL by its own path).
2. Edit `myosotis.ini`:

   ```ini
   [myosotis]
   token=<your hardcoded steam JWT>
   server=http://127.0.0.1:3000/
   serverinfos_url=https://raw.githubusercontent.com/LEAGUE-OF-NINE/motions-schema/refs/heads/main/serverinfos.json
   log_level=1
   ```

   The first run creates a default `myosotis.ini` with these keys if none exists.
3. Launch Limbus Company through Steam (normally — let it pass the launcher
   anti-cheat on its own).
4. Run the loader:

   ```bat
   myoink
   ```

   `myoink` polls up to 5 minutes for a top-level window whose title exactly
   equals "LimbusCompany" (exact match avoids hitting the launcher process,
   which rejects `CreateRemoteThread` with `ACCESS_DENIED`). Once the window
   appears, it injects `myosotis.dll` via `CreateRemoteThread(LoadLibraryW,
   <remote path>)`. The DLL's init thread polls for `GameAssembly.dll` +
   `il2cpp_init` done, then installs all patches. Late injection works because
   the init thread waits for the il2cpp runtime regardless of when the DLL
   loads.

   The DLL writes `myosotis.log` next to itself — since `myoink` loads
   `myosotis.dll` from next to the loader, the log lands in the myoink
   directory, not the game directory.

   On load the DLL pops a console window (titled "myosotis") and logs there
   simultaneously with `OutputDebugStringW` and the log file. Watch the console
   for `[Myosotis:hook] inline hooked ...` lines confirming each patch, and
   `... FIRED` lines when the game hits a hooked method.

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
    log.cpp           console + OutputDebugStringW + file log
    config.cpp        myosotis.ini via GetPrivateProfileStringW
    hook.cpp          inline (native-body) hook helper with trampoline
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
    loader.cpp        myoink — attach-mode injector (no args)
    test_scan.cpp     standalone scan self-test
    regen_map.py      regenerates generated/ from UnityPlayer.dll
    analyze_mapping.py copied from la-ng — the offline resolver regen_map.py drives
    server.py         logging HTTP server for local dev (127.0.0.1:3000)
  build.bat           build driver — `build.bat [dll|loader|test|all]`
  .gitignore          ignores build/ and generated/
```

## Known limitations / TODO

- **Request patch `apiPath` resolution.** The C# version calls the schema's
  `get_ApiClass` (returns an enum) and lowercases `enum.ToString()`. We resolve
  the enum value's integer but haven't wired up `System.Enum.GetName` to recover
  the member name — so the `path → packetId` map keys currently use the string
  part of the API path only. This needs `System.Enum.GetName(Type, object)` and
  `Type.GetTypeFromHandle` wired through the il2cpp bridge to be fully faithful.
  See `build_api_path` in `src/patches/request.cpp`.
- **AuthTicket / SteamId field offsets** are hardcoded (0x10 after the object
  header) rather than resolved via `il2cpp_class_get_field_from_name`. Robust
  against the current build; would need the field-by-name path if Steamworks.NET
  layout changes.
- **`_responseEvent` field offset** on `HttpApiSchema` is hardcoded to 0x18.
  Same caveat — should be resolved by field name for long-term stability.
- **Inline hook length decoder** handles the common il2cpp prologue opcodes but
  is not a full disassembler. Methods whose prologue uses an unsupported opcode
  within the first 5 bytes fall back to `methodPointer`-only (logged as
  `inline hook skipped`). Extending `insn_len` covers more; a full LDE would
  cover all.
