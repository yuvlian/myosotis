// WinHTTP wrapper implementation.
#include "http.hpp"
#include "log.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <string_view>
#include <cstring>


namespace myosotis::http {

namespace {

struct UrlParts {
    std::wstring host;
    std::wstring path;           // includes query
    int port = 0;                 // 0 => default
    bool https = true;
};

// Narrow -> wide via UTF-8.
std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Parse a URL via std::string_view (no C-string strchr/strlen dance).
// Accepts scheme://host[:port]/path?query.
bool parse_url(std::string_view url, UrlParts& out) {
    auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) return false;
    auto scheme = url.substr(0, scheme_end);
    out.https = (scheme == "https");
    auto rest = url.substr(scheme_end + 3);
    auto slash = rest.find('/');
    auto authority = (slash == std::string_view::npos) ? rest : rest.substr(0, slash);
    auto path_view = (slash == std::string_view::npos) ? std::string_view{"/"} : rest.substr(slash);

    auto colon = authority.find(':');
    if (colon != std::string_view::npos) {
        out.host = widen(authority.substr(0, colon));
        // atoi replacement: std::stoi on the substring.
        try { out.port = std::stoi(std::string{authority.substr(colon + 1)}); }
        catch (...) { out.port = out.https ? 443 : 80; }
    } else {
        out.host = widen(authority);
        out.port = out.https ? 443 : 80;
    }
    out.path = widen(path_view);
    return true;
}

// RAII guard for the three WinHTTP handles a request opens. Releases in reverse
// order on scope exit; collapses the early-return ladders in do_request.
struct WinHttpScope {
    HINTERNET session = nullptr;
    HINTERNET conn    = nullptr;
    HINTERNET req     = nullptr;
    ~WinHttpScope() {
        if (req)     WinHttpCloseHandle(req);
        if (conn)    WinHttpCloseHandle(conn);
        if (session) WinHttpCloseHandle(session);
    }
};

Response do_request(std::string_view url, std::string_view method,
                    const std::string& body,
                    const std::string& expected_packet_id) {
    Response r;
    UrlParts u;
    if (!parse_url(url, u)) { r.error = "bad url"; return r; }

    WinHttpScope s;
    s.session = WinHttpOpen(L"Myosotis/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s.session) { r.error = "WinHttpOpen"; return r; }

    s.conn = WinHttpConnect(s.session, u.host.c_str(),
        static_cast<INTERNET_PORT>(u.port), 0);
    if (!s.conn) { r.error = "WinHttpConnect"; return r; }

    s.req = WinHttpOpenRequest(s.conn, widen(method).c_str(), u.path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        u.https ? WINHTTP_FLAG_SECURE : 0);
    if (!s.req) { r.error = "WinHttpOpenRequest"; return r; }

    // Headers
    if (!body.empty()) {
        WinHttpAddRequestHeaders(s.req, L"Content-Type: application/json\r\n",
            static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);
    }
    if (!expected_packet_id.empty()) {
        std::wstring h = L"X-Expected-Packet-Id: " + widen(expected_packet_id) + L"\r\n";
        WinHttpAddRequestHeaders(s.req, h.c_str(), static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);
    }

    BOOL sent = WinHttpSendRequest(s.req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    if (!sent) { r.error = "WinHttpSendRequest"; return r; }

    if (!WinHttpReceiveResponse(s.req, nullptr)) { r.error = "WinHttpReceiveResponse"; return r; }

    // Status code
    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(s.req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    r.status = static_cast<int>(status);

    // Body
    DWORD avail = 0;
    do {
        avail = 0;
        if (!WinHttpQueryDataAvailable(s.req, &avail)) break;
        if (avail == 0) break;
        size_t off = r.body.size();
        r.body.resize(off + avail);
        if (!WinHttpReadData(s.req, r.body.data() + off, avail, nullptr)) {
            r.body.resize(off);
            break;
        }
    } while (avail > 0);

    return r;
}

}  // namespace

Response post(const std::string& url, const std::string& body,
              const std::string& expected_packet_id) {
    return do_request(url, "POST", body, expected_packet_id);
}

Response get(const std::string& url) {
    return do_request(url, "GET", std::string{}, std::string{});
}

}  // namespace myosotis::http
