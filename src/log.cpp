// Logging implementation: OutputDebugStringW.
#include "log.hpp"
#include <windows.h>
#include <string>

namespace myosotis {

void log_init() {
    OutputDebugStringW(L"[Myosotis] logging via OutputDebugStringW\n");
}

void log_raw(const char* msg) {
    if (!msg) return;
    std::string s(msg);
    s.push_back('\n');
    // Widen to UTF-16 for OutputDebugStringW (most reliable on Windows).
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
}

}  // namespace myosotis
