// Logging implementation: OutputDebugStringW + file log.
//
// The file log is the primary diagnostic surface — OutputDebugStringW requires
// DebugView to observe, which most users won't have running. We append every
// line to myosotis.log next to the DLL, so a failed init leaves a visible trail.

#include "log.hpp"
#include <windows.h>
#include <string>
#include <string_view>
#include <cstdio>

namespace myosotis {

namespace {

// Path of the log file (DLL dir + "myosotis.log"). Cached after first use.
std::wstring g_log_path;
CRITICAL_SECTION g_log_lock;
bool g_log_lock_inited = false;
bool g_have_console = false;

void ensure_log_path() {
    if (!g_log_path.empty()) return;
    if (!g_log_lock_inited) {
        InitializeCriticalSection(&g_log_lock);
        g_log_lock_inited = true;
    }
    wchar_t exe[MAX_PATH] = {};
    HMODULE h = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ensure_log_path), &h);
    if (h && GetModuleFileNameW(h, exe, MAX_PATH)) {
        std::wstring p(exe);
        auto slash = p.find_last_of(L"\\/");
        if (slash != std::wstring::npos) p.resize(slash + 1);
        else p.clear();
        g_log_path = p + L"myosotis.log";
    } else {
        g_log_path = L"myosotis.log";
    }
}

void write_file(const std::string& line) {
    ensure_log_path();
    EnterCriticalSection(&g_log_lock);
    if (HANDLE h = CreateFileW(g_log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        h != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &wr, nullptr);
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_log_lock);
}

// Allocate a console and redirect stdout/stderr to it. Safe to call once; a
// console already exists (e.g. launched from a terminal) is a no-op via the
// AttachConsole fallback. Writes go through the C runtime, so we freopen the
// handles after AllocConsole so printf/fputs reach the new console buffer.
void alloc_console() {
    if (g_have_console) return;
    if (AllocConsole()) {
        g_have_console = true;
    } else if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        // Already have a parent console; attach to it so our output shows up.
        g_have_console = true;
    } else {
        return;  // no console available; file log still works
    }
    // Redirect C stdio to the console so fputs/printf land there.
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
    SetConsoleTitleW(L"myosotis");
}

void write_console(const std::string& line) {
    if (!g_have_console) return;
    fputs(line.c_str(), stdout);
    fflush(stdout);
}

}  // namespace

void log_init() {
    alloc_console();
    OutputDebugStringW(L"[Myosotis] logging via OutputDebugStringW + myosotis.log + console\n");
    // Truncate the log on each fresh load so you don't see stale output.
    ensure_log_path();
    EnterCriticalSection(&g_log_lock);
    if (HANDLE h = CreateFileW(g_log_path.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        h != INVALID_HANDLE_VALUE) CloseHandle(h);
    LeaveCriticalSection(&g_log_lock);
    write_file("[Myosotis] log started\n");
}

void log_raw(const char* msg) {
    if (!msg) return;
    std::string s(msg);
    s.push_back('\n');
    write_file(s);
    write_console(s);
    // Also emit to OutputDebugStringW for debugger/DebugView users.
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) { OutputDebugStringA(s.c_str()); return; }
    std::wstring w(static_cast<size_t>(n), 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    OutputDebugStringW(w.c_str());
}

void log_raw(const wchar_t* msg) {
    if (!msg) return;
    OutputDebugStringW(msg);
    OutputDebugStringW(L"\n");
    // Also widen-and-append to the file log so wide-char calls are captured.
    std::string narrow;
    if (int n = WideCharToMultiByte(CP_UTF8, 0, msg, -1, nullptr, 0, nullptr, nullptr); n > 0) {
        narrow.resize(static_cast<size_t>(n - 1));
        WideCharToMultiByte(CP_UTF8, 0, msg, -1, narrow.data(), n, nullptr, nullptr);
        narrow.push_back('\n');
        write_file(narrow);
        write_console(narrow);
    }
}

}  // namespace myosotis
