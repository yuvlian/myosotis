// myosotis-loader: launch Limbus Company suspended, inject myosotis.dll, resume.
//
// Usage:
//   myosotis-loader.exe <game.exe> <myosotis.dll> [args...]
//
// Flow:
//   1. CreateProcessW(game.exe, CREATE_SUSPENDED) — process is alive but its
//      main thread hasn't run a single instruction yet.
//   2. VirtualAllocEx a page in the target for the DLL path (UTF-16, NUL-term).
//   3. Write the absolute path of myosotis.dll into that page.
//   4. CreateRemoteThread(LoadLibraryW, arg = remote path page). Wait for it.
//      LoadLibraryW runs in the target; our DllMain spawns the init thread and
//      returns immediately, so the remote thread finishes once the DLL is
//      mapped and DllMain has returned.
//   5. ResumeThread on the suspended main thread — the game starts running with
//      our DLL already loaded. Our init thread polls for GameAssembly.dll +
//      il2cpp domain ready, then installs patches.
//
// We resolve LoadLibraryW by GetProcAddress(kernel32, "LoadLibraryW") — kernel32
// is mapped at a fixed base across processes on the same Windows boot, so the
// address we resolve locally is valid in the target.

#include <string>
#include <vector>
#include <format>
#include <windows.h>
#include <cstdio>

namespace {

// RAII guard for the suspended-process + remote-thread handles. Closing both
// on every early return used to be a manual ladder; this collapses it.
struct ProcGuard {
    HANDLE process = nullptr;
    HANDLE thread  = nullptr;
    bool    keep   = false;   // set true on success path so we don't kill the proc
    ~ProcGuard() {
        if (thread)  CloseHandle(thread);
        if (process) {
            if (!keep) TerminateProcess(process, 1);
            CloseHandle(process);
        }
    }
};

void fail(const char* msg, DWORD err = 0) {
    if (err) std::fprintf(stderr, "[loader] %s (err=%lu)\n", msg, err);
    else     std::fprintf(stderr, "[loader] %s\n", msg);
}

// Absolute path of `rel` (resolves relative to cwd). Empty on failure.
std::wstring abs_path(const wchar_t* rel) {
    DWORD n = GetFullPathNameW(rel, 0, nullptr, nullptr);
    if (n == 0) return {};
    std::wstring out(n, 0);
    DWORD m = GetFullPathNameW(rel, n, out.data(), nullptr);
    if (m == 0 || m >= n) return {};
    out.resize(m);
    return out;
}

// Quote a single argv token if it contains whitespace.
std::wstring quote_if_needed(std::wstring_view s) {
    if (s.find_first_of(L" \t") != std::wstring_view::npos)
        return std::format(L"\"{}\"", s);
    return std::wstring{s};
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: myosotis-loader.exe <game.exe> <myosotis.dll> [args...]\n");
        return 1;
    }
    const wchar_t* game_exe = argv[1];
    const wchar_t* dll_rel  = argv[2];

    std::wstring dll_path = abs_path(dll_rel);
    if (dll_path.empty()) { fail("could not resolve dll absolute path"); return 2; }

    // Build the command line: quoted game.exe + any extra args.
    std::wstring cmdline = quote_if_needed(abs_path(game_exe));
    if (cmdline.empty()) { fail("could not resolve game absolute path"); return 2; }
    for (int i = 3; i < argc; ++i) {
        cmdline += L" ";
        cmdline += quote_if_needed(argv[i]);
    }

    std::vector<wchar_t> clbuf(cmdline.begin(), cmdline.end());
    clbuf.push_back(0);

    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    ProcGuard guard;

    if (!CreateProcessW(nullptr, clbuf.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        fail("CreateProcessW failed", GetLastError());
        return 3;
    }
    guard.process = pi.hProcess;
    guard.thread  = pi.hThread;
    std::fprintf(stderr, "[loader] process created suspended, pid=%lu\n", pi.dwProcessId);

    // Allocate room for the DLL path in the target.
    size_t path_bytes = (dll_path.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(pi.hProcess, nullptr, path_bytes,
                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { fail("VirtualAllocEx failed", GetLastError()); return 4; }

    SIZE_T written = 0;
    if (!WriteProcessMemory(pi.hProcess, remote, dll_path.data(), path_bytes, &written) ||
        written != path_bytes) {
        fail("WriteProcessMemory failed", GetLastError());
        VirtualFreeEx(pi.hProcess, remote, 0, MEM_RELEASE);
        return 5;
    }

    // Resolve LoadLibraryW. kernel32 is mapped identically in every process.
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) { fail("kernel32 not loaded"); return 6; }
    auto load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(k32, "LoadLibraryW"));
    if (!load_library) { fail("LoadLibraryW not found"); return 7; }

    // Spawn a remote thread that runs LoadLibraryW(remote_path).
    HANDLE rt = CreateRemoteThread(pi.hProcess, nullptr, 0,
                                   load_library, remote, 0, nullptr);
    if (!rt) { fail("CreateRemoteThread failed", GetLastError()); VirtualFreeEx(pi.hProcess, remote, 0, MEM_RELEASE); return 8; }

    if (WaitForSingleObject(rt, 30000) != WAIT_OBJECT_0) {
        fail("remote LoadLibraryW timed out");
    }
    DWORD exit_code = 0;
    GetExitCodeThread(rt, &exit_code);
    std::fprintf(stderr, "[loader] LoadLibraryW returned 0x%08lx\n", exit_code);

    CloseHandle(rt);
    VirtualFreeEx(pi.hProcess, remote, 0, MEM_RELEASE);

    if (exit_code == 0) { fail("DLL load failed in target"); return 9; }
    std::fprintf(stderr, "[loader] DLL loaded, resuming main thread\n");

    if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
        fail("ResumeThread failed", GetLastError());
        return 10;
    }

    guard.keep = true;  // don't terminate the now-resumed game
    std::fprintf(stderr, "[loader] done\n");
    return 0;
}
