# myosotis

a native port of the subset of lethe bepinex plugin that can be built with zig 0.17 at c++23

## what r the patches

1. **guard** - this is originally to neutralize `JsonExtensions` (which is limbus anticheat thing) but its obsolete cuz they removed it. its good to keep around tho for future references.

2. **login** - patches the steam token for the private server to actually process. as of right now, im simply hardcoding it.

3. **http** - redirect http requests except for cdn requests

4. **request** - normally limbus requests are encrypted and such. packets also have packetids (one for request, one for response). to resolve these annoyances for PS development, we simply replace the game's http stack with our own using WinHTTP. the packetid in the request body will be instead a packetid for the response. the patch also injects x-expected-packet-id header if you prefer parsing from there.

## building

requires zig 0.17 or newer

```bat
build.bat            :: build everything
build.bat dll        :: build myosotis.dll only
build.bat loader     :: build myoink.exe loader only
build.bat test       :: build test_scan.exe only
```

build output will be in `./build`

## usage

1. after you build everything, copy the `myosotis.ini` to build dir or whatever
2. configure that as you need
3. run myoink
4. open limbus

## how le hooks work

il2cpp's managed-to-managed calls use baked-in direct native call addresses

the compiled caller jumps straight to the method's native body, never reading
`MethodInfo->methodPointer`

overwriting `methodPointer` alone only intercepts
`runtime_invoke` based calls (reflection)

we solve this with inline hooking. patch the native code body at `methodPointer` with a jump to our stub, so both direct managed calls and `runtime_invoke` paths are intercepted

but, some hooks need to call the original after running (e.g. `SendWebRequest`
returns a coroutine the caller needs, `Post` must actually send the request), we build a trampoline

two patch sizes are used:
- 5-byte relative jump (`E9 rel32`) when possible,
- 12-byte absolute jump (`mov rax, imm64; jmp rax`) otherwise.

if the length decoder can't safely cover the patch size (unknown instruction before the boundary), the inline patch is skipped

## how are il2cpp export names resolved

limbus uses unity 6000.3.12f1. they ship the `GameAssembly.dll` with 234 il2cpp C API exports obfuscated to random 11-char names.

the canonical -> obfuscated ordering is encoded only in the order of `LEA [rip+disp32]` instructions inside `LoadIl2CPP` in `UnityPlayer.dll`

1. load `UnityPlayer.dll` from disk into a virtual image buffer.
2. walk `UnityMain -> body -> impl -> LoadIl2CPP` via `E8 rel32` CALL scans
3. collect every rip-relative `LEA` whose target is an 11-char ASCII string, in instruction order, dedup by first occurrence.
4. assert the count equals 234 and zip with `IL2CPP_NAMES`.

## project structure

```
cpp/
├── build/                  (gitignored) default build output dir
├── generated/              (gitignored) for generated fallback il2cpp mappings
├── include/
│   └── patches/            header files for the patches
├── src/
│   ├── dllmain.cpp         dll entrypoint
│   ├── init.cpp            dll patches initializer
│   ├── log.cpp             file & console logger
│   ├── config.cpp          myosotis.ini stuff
│   ├── hook.cpp            inline hook helper with trampoline
│   ├── http.cpp            synchronous WinHTTP wrapper
│   ├── il2cpp/
│   │   ├── pe.cpp          PE parser
│   │   ├── scan.cpp        byte-pattern CALL/LEA scanner
│   │   ├── il2cpp_names.cpp canonical -> obfuscated resolver
│   │   └── il2cpp.cpp      typed il2cpp C API bridge
│   └── patches/
│       ├── guard.cpp       anti-cheat neutralization
│       ├── login.cpp       steam token patcher
│       ├── http.cpp        for http request redirect
│       └── request.cpp     make the game use custom http stack
├── tools/
│   ├── loader.cpp          injector (myoink.exe)
│   ├── test_scan.cpp       test name resolver
│   ├── regen_map.py        fallback map generator (kinda useless)
│   └── server.py           http server to view first few requests
├── build.bat               build script
├── myosotis.ini            example myosotis.ini config
└── .gitignore              self explanatory
```

## limitations
- no android support or whatever (boooooo, i know)

- inline hook length decoder handles the common il2cpp prologue opcodes but
  is not a full disassembler

- request patch is "blocking" bla bla bla idc i hate async
