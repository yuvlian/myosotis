// myoink: inject a DLL into a running Limbus Company process.
//
// Usage: myoink [dump]
//
// No arguments  → injects myosotis.dll (the patch DLL).
// "dump"        → injects myodump.dll (the il2cpp dumper).
//
// Looks for the DLL in the same directory as this exe (so the loader and DLLs
// can live anywhere). Polls up to 5 minutes for a top-level window whose title
// exactly equals "LimbusCompany", opens its process, and injects via
// VirtualAllocEx + CreateRemoteThread(LoadLibraryW). Launch Limbus through
// Steam first; the launcher's anti-cheat has already passed by the time the
// window is up, so attaching to the windowed process works (matching the
// process image name catches the launcher, which has ACCESS_DENIED on
// CreateRemoteThread).
//
// Each DLL writes its log file next to itself (the DLL is loaded from wherever
// myoink found it). myodump also writes ./dump/ next to itself.
//
// LoadLibraryW is resolved via kernel32 (fixed base across processes per boot).

#include <string>
#include <windows.h>
#include <cstdio>
#include <cwchar>

namespace {

constexpr const wchar_t* kWindowTitle = L"LimbusCompany";
constexpr const wchar_t* kDllName     = L"myosotis.dll";
constexpr const wchar_t* kDumpDllName = L"myodump.dll";
constexpr int kWaitPollMs   = 500;
constexpr int kWaitMaxPolls = 600;  // 600 * 500ms = 5 minutes

void fail(const char* msg, DWORD err = 0) {
    if (err) std::fprintf(stderr, "[myoink] %s (err=%lu)\n", msg, err);
    else     std::fprintf(stderr, "[myoink] %s\n", msg);
}

// Absolute directory of this exe, with trailing backslash. Empty on failure.
std::wstring exe_dir() {
    wchar_t path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring p(path, n);
    size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return p.substr(0, slash + 1);
}

// Absolute path of `rel` (resolved against cwd). Empty on failure.
std::wstring abs_path(const std::wstring& rel) {
    DWORD n = GetFullPathNameW(rel.c_str(), 0, nullptr, nullptr);
    if (n == 0) return {};
    std::wstring out(n, 0);
    DWORD m = GetFullPathNameW(rel.c_str(), n, out.data(), nullptr);
    if (m == 0 || m >= n) return {};
    out.resize(m);
    return out;
}

// Find a top-level window whose title EXACTLY equals `title` (case-insensitive).
// Exact match avoids accidentally matching a cmd window whose title contains
// the command line you just typed. Returns the HWND or nullptr.
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
HWND find_window_by_title(const wchar_t* title) {
    FindWnd f{title, nullptr};
    EnumWindows(&find_wnd_enum, reinterpret_cast<LPARAM>(&f));
    return f.hwnd;
}

// Inject `dll_path` into `proc` via a remote LoadLibraryW thread.
bool inject_dll(HANDLE proc, const std::wstring& dll_path) {
    size_t path_bytes = (dll_path.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, path_bytes, MEM_COMMIT | MEM_RESERVE,
                                 PAGE_READWRITE);
    if (!remote) { fail("VirtualAllocEx failed", GetLastError()); return false; }

    SIZE_T written = 0;
    if (!WriteProcessMemory(proc, remote, dll_path.data(), path_bytes, &written) || written != path_bytes) {
        fail("WriteProcessMemory failed", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return false;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) { fail("kernel32 not loaded"); VirtualFreeEx(proc, remote, 0, MEM_RELEASE); return false; }
    auto load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(k32, "LoadLibraryW"));
    if (!load_library) { fail("LoadLibraryW not found"); VirtualFreeEx(proc, remote, 0, MEM_RELEASE); return false; }

    HANDLE rt = CreateRemoteThread(proc, nullptr, 0, load_library, remote, 0, nullptr);
    if (!rt) { fail("CreateRemoteThread failed", GetLastError()); VirtualFreeEx(proc, remote, 0, MEM_RELEASE); return false; }
    bool loaded = false;
    if (WaitForSingleObject(rt, 30000) == WAIT_OBJECT_0) {
        DWORD ec = 0; GetExitCodeThread(rt, &ec);
        std::fprintf(stderr, "[myoink] LoadLibraryW returned 0x%08lx\n", ec);
        loaded = (ec != 0);
    } else {
        fail("remote LoadLibraryW timed out");
    }
    CloseHandle(rt);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    return loaded;
}

}  // namespace
int wmain(int argc, wchar_t** argv) {
    // Pick the DLL: "dump" arg → myodump.dll, else myosotis.dll.
    const wchar_t* dll_name = kDllName;
    if (argc >= 2 && std::wcscmp(argv[1], L"dump") == 0) {
        dll_name = kDumpDllName;
        std::fprintf(stderr, "[myoink] mode: dump\n");
    } else if (argc >= 2) {
        fail("unknown argument (expected: dump)");
        return 1;
    }

    // Resolve the DLL: next to this exe, or in the cwd as a fallback.
    std::wstring dir = exe_dir();
    std::wstring dll_rel = dir.empty() ? std::wstring(dll_name) : (dir + dll_name);
    std::wstring dll = abs_path(dll_rel);
    if (dll.empty() || GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        dll = abs_path(std::wstring(dll_name));  // fallback: cwd
    }
    if (dll.empty() || GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        fail("could not find the DLL next to myoink.exe");
        return 2;
    }
    std::fprintf(stderr, "[myoink] dll: %ls\n", dll.c_str());
    std::fprintf(stderr, "[myoink] waiting for window \"%ls\"...\n", kWindowTitle);
    HWND hwnd = nullptr;
    DWORD pid = 0;
    for (int i = 0; i < kWaitMaxPolls; ++i) {
        if ((hwnd = find_window_by_title(kWindowTitle)) != nullptr) {
            GetWindowThreadProcessId(hwnd, &pid);
            break;
        }
        Sleep(kWaitPollMs);
    }
    if (!pid) { fail("timed out waiting for LimbusCompany window"); return 20; }
    wchar_t title[256] = {};
    GetWindowTextW(hwnd, title, 256);
    std::fprintf(stderr, "[myoink] found window: \"%ls\" pid=%lu\n", title, pid);

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                              PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                              PROCESS_VM_READ, FALSE, pid);
    if (!proc) { fail("OpenProcess failed", GetLastError()); return 22; }

    bool ok = inject_dll(proc, dll);
    CloseHandle(proc);
    if (!ok) { fail("injection failed"); return 23; }
    std::fprintf(stderr, "[myoink] injected, myoinking it!\n");
    return 0;
}
