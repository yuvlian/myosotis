// Config implementation.
#include "config.hpp"
#include "log.hpp"
#include <windows.h>
#include <string>

namespace myosotis::config {

Config g;

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == 0) s.pop_back();
    return s;
}
namespace {

std::wstring dll_dir() {
    wchar_t path[MAX_PATH] = {};
    HMODULE h = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&dll_dir), &h);
    if (!h) return L".\\";
    if (!GetModuleFileNameW(h, path, MAX_PATH)) return L".\\";
    std::wstring p(path);
    size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L".\\";
    return p.substr(0, slash + 1);
}

std::wstring read_ini(const wchar_t* ini_path, const wchar_t* section,
                      const wchar_t* key, const wchar_t* def) {
    wchar_t buf[1024] = {};
    GetPrivateProfileStringW(section, key, def, buf, 1024, ini_path);
    return std::wstring(buf);
}

}  // namespace

bool load() {
    std::wstring dir = dll_dir();
    std::wstring ini = dir + L"myosotis.ini";

    // Write a default ini if none exists so the user can see the keys.
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const wchar_t* def =
            L"[myosotis]\r\n"
            L"; Hardcoded steam JWT used as the auth ticket (replaces browser login)\r\n"
            L"token=\r\n"
            L"; Server to redirect API requests to (must include scheme + trailing slash)\r\n"
            L"server=https://api.lethelc.site/\r\n"
            L"; URL to serve for /serverinfos_* requests\r\n"
            L"serverinfos_url=https://raw.githubusercontent.com/LEAGUE-OF-NINE/motions-schema/refs/heads/main/serverinfos.json\r\n"
            L"log_level=1\r\n";
        HANDLE h = CreateFileW(ini.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD wr = 0;
            WriteFile(h, def, static_cast<DWORD>(wcslen(def) * sizeof(wchar_t)), &wr, nullptr);
            CloseHandle(h);
        }
    }

    g.token           = read_ini(ini.c_str(), L"myosotis", L"token", L"");
    g.server          = read_ini(ini.c_str(), L"myosotis", L"server", L"https://api.lethelc.site/");
    g.serverinfos_url = read_ini(ini.c_str(), L"myosotis", L"serverinfos_url",
                                 L"https://raw.githubusercontent.com/LEAGUE-OF-NINE/motions-schema/refs/heads/main/serverinfos.json");
    g.log_level       = static_cast<int>(GetPrivateProfileIntW(L"myosotis", L"log_level", 1, ini.c_str()));

    MYO_LOG("config", "server={}", narrow(g.server));
    return true;
}

}  // namespace myosotis::config
