// myosotis-loader: inject myosotis.dll into Limbus Company.
//
// Two modes:
//
// 1. Suspended-launch (default):
//    myosotis-loader.exe <game.exe> <myosotis.dll> [args...]
//    CreateProcessW(game.exe, CREATE_SUSPENDED), inject, resume. Fast and
//    deterministic, but LimbusCompany.exe is a self-relaunching launcher whose
//    native anti-cheat detects the suspended start and exits code 53 before
//    il2cpp loads — so the DLL dies with the launcher. Use this only if the
//    game ever ships a non-relaunching exe.
//
// 2. Attach to running window (--attach):
//    myosotis-loader.exe --attach <window_title> <myosotis.dll>
//    Poll until a top-level window whose title contains <window_title> exists,
//    open its process, and inject via VirtualAllocEx + CreateRemoteThread. The
//    game launches normally through Steam (anti-cheat has already passed by the
//    time the window is up), and we attach after the fact. Our init thread polls
//    for GameAssembly.dll + il2cpp domain ready, so late injection works fine —
//    the game's already past the launcher and loading the il2cpp runtime.
//
// LoadLibraryW is resolved via kernel32 (fixed base across processes per boot).

#include <string>
#include <vector>
#include <format>
#include <windows.h>
#include <cstdio>
#include <tlhelp32.h>
#include <cwchar>

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

// Inject `dll_path` into an already-opened process `proc` via a remote
// LoadLibraryW thread. Returns true on success. Closes the remote thread.
bool inject_into_process(HANDLE proc, const std::wstring& dll_path) {
    size_t path_bytes = (dll_path.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, path_bytes,
                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { fail("VirtualAllocEx failed", GetLastError()); return false; }

    SIZE_T written = 0;
    bool ok = WriteProcessMemory(proc, remote, dll_path.data(), path_bytes, &written) &&
               written == path_bytes;
    if (!ok) {
        fail("WriteProcessMemory failed", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return false;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) { fail("kernel32 not loaded"); VirtualFreeEx(proc, remote, 0, MEM_RELEASE); return false; }
    auto load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(k32, "LoadLibraryW"));
    if (!load_library) { fail("LoadLibraryW not found"); VirtualFreeEx(proc, remote, 0, MEM_RELEASE); return false; }

    HANDLE rt = CreateRemoteThread(proc, nullptr, 0, load_library, remote, 0, nullptr);
    if (!rt) {
        fail("CreateRemoteThread failed", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return false;
    }
    bool loaded = false;
    if (WaitForSingleObject(rt, 30000) == WAIT_OBJECT_0) {
        DWORD ec = 0; GetExitCodeThread(rt, &ec);
        std::fprintf(stderr, "[loader] LoadLibraryW returned 0x%08lx\n", ec);
        loaded = (ec != 0);
    } else {
        fail("remote LoadLibraryW timed out");
    }
    CloseHandle(rt);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    return loaded;
}

// Find a top-level window whose title EXACTLY equals `needle` (case-insensitive).
// Exact match avoids accidentally matching a cmd window whose title contains the
// command line you just typed.
struct FindWnd { const wchar_t* needle; HWND hwnd; };
BOOL CALLBACK find_wnd_enum(HWND hwnd, LPARAM lp) {
    auto* f = reinterpret_cast<FindWnd*>(lp);
    wchar_t title[256] = {};
    if (GetWindowTextW(hwnd, title, 256) > 0) {
        if (_wcsicmp(title, f->needle) == 0) {
            f->hwnd = hwnd;
            return FALSE;
        }
    }
    return TRUE;
}
HWND find_window(const wchar_t* title_needle) {
    FindWnd f{title_needle, nullptr};
    EnumWindows(&find_wnd_enum, reinterpret_cast<LPARAM>(&f));
    return f.hwnd;
}

// Find the PID of a running process whose image name equals `exe_name`
// (case-insensitive). Uses the toolhelp snapshot. Returns 0 on no match.
DWORD find_process_by_name(const wchar_t* exe_name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    DWORD pid = 0;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exe_name) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

// Attach mode: wait for a target identified by `target`, inject myosotis.dll.
// `target` is either "<window_title>" (exact match) or "<name>.exe" (process
// image name via toolhelp snapshot).
int run_attach(const wchar_t* target, const std::wstring& dll_path) {
    bool is_process = false;
    std::wstring s(target);
    if (s.size() >= 4 && _wcsicmp(s.c_str() + s.size() - 4, L".exe") == 0) {
        is_process = true;
    }

    std::fprintf(stderr, "[loader] attach mode: waiting for %ls \"%ls\"...\n",
                 is_process ? L"process" : L"window", target);
    HWND hwnd = nullptr;
    DWORD pid = 0;
    for (int i = 0; i < 600; ++i) {  // up to 60s
        if (is_process) {
            if ((pid = find_process_by_name(target)) != 0) break;
        } else {
            if ((hwnd = find_window(target)) != nullptr) {
                GetWindowThreadProcessId(hwnd, &pid);
                break;
            }
        }
        Sleep(100);
    }
    if (!pid) { fail("timed out waiting for target"); return 20; }

    if (hwnd) {
        wchar_t title[256] = {};
        GetWindowTextW(hwnd, title, 256);
        std::fprintf(stderr, "[loader] found window: \"%ls\"\n", title);
    } else {
        std::fprintf(stderr, "[loader] found process: %ls\n", target);
    }
    std::fprintf(stderr, "[loader] target pid=%lu\n", pid);

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                              PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                              PROCESS_VM_READ, FALSE, pid);
    if (!proc) { fail("OpenProcess failed", GetLastError()); return 22; }

    bool ok = inject_into_process(proc, dll_path);
    CloseHandle(proc);
    if (!ok) { fail("injection failed"); return 23; }
    std::fprintf(stderr, "[loader] injected, myosotis running in target\n");
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // Attach mode: --attach <window_title> <myosotis.dll>
    if (argc >= 4 && std::wcscmp(argv[1], L"--attach") == 0) {
        const wchar_t* title = argv[2];
        const wchar_t* dll_rel = argv[3];
        std::wstring dll_path = abs_path(dll_rel);
        if (dll_path.empty()) { fail("could not resolve dll absolute path"); return 2; }
        return run_attach(title, dll_path);
    }

    if (argc < 3) {
        std::fprintf(stderr, "usage:\n");
        std::fprintf(stderr, "  myosotis-loader.exe <game.exe> <myosotis.dll> [args...]      (suspended-launch)\n");
        std::fprintf(stderr, "  myosotis-loader.exe --attach <window_title> <myosotis.dll>   (inject into running game)\n");
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

    if (!inject_into_process(pi.hProcess, dll_path)) {
        return 9;
    }
    std::fprintf(stderr, "[loader] DLL loaded, resuming main thread\n");

    if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
        fail("ResumeThread failed", GetLastError());
        return 10;
    }
    guard.keep = true;  // don't kill the now-resumed game
    std::fprintf(stderr, "[loader] done\n");

    // Wait briefly to detect a self-relaunching launcher. If the game process
    // exits within 5s, it almost certainly spawned a child and quit — our DLL
    // died with it. (LimbusCompany.exe is known to do this under Steam.)
    if (WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0) {
        DWORD ec = 0; GetExitCodeProcess(pi.hProcess, &ec);
        std::fprintf(stderr, "[loader] WARNING: game process exited quickly (code %lu) — likely a self-relaunching launcher.\n", ec);
        std::fprintf(stderr, "[loader] The injected DLL died with the launcher. If the game is now running, it's a separate process without myosotis.\n");
        std::fprintf(stderr, "[loader] Try attach mode: myosotis-loader.exe --attach LimbusCompany myosotis.dll\n");
    }
    return 0;
}
