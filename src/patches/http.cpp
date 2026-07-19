// Http patch implementation.
#include "patches/http.hpp"
#include <windows.h>
#include "hook.hpp"
#include "il2cpp.hpp"
#include "config.hpp"
#include "log.hpp"
#include <string>
#include <cstring>

namespace myosotis::patches {

namespace {

// Globals for hook trampolines and saved method info.
il2cpp::Il2CppMethod* g_post_method = nullptr;
void* g_post_original = nullptr;
void* g_send_original = nullptr;
il2cpp::Il2CppMethod* g_send_method = nullptr;
using Send_t = il2cpp::Il2CppObject* (*)(il2cpp::Il2CppObject* self, il2cpp::Il2CppMethod* method_info);
using Post_t = il2cpp::Il2CppObject* (*)(il2cpp::Il2CppString* uri, il2cpp::Il2CppString* body,
                                          il2cpp::Il2CppMethod* method_info);

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == 0) s.pop_back();
    return s;
}

bool url_host_scheme(const std::string& url, std::string& scheme, std::string& host) {
    auto pos = url.find("://");
    if (pos == std::string::npos) return false;
    scheme = url.substr(0, pos);
    size_t p = pos + 3;
    size_t slash = url.find('/', p);
    host = url.substr(p, slash == std::string::npos ? std::string::npos : slash - p);
    auto colon = host.find(':');
    if (colon != std::string::npos) host = host.substr(0, colon);
    return true;
}

std::string url_replace_host_scheme(const std::string& original, const std::string& target) {
    std::string t_scheme, t_host;
    if (!url_host_scheme(target, t_scheme, t_host)) return original;
    auto pos = original.find("://");
    if (pos == std::string::npos) return original;
    size_t p = pos + 3;
    size_t slash = original.find('/', p);
    std::string rest = (slash == std::string::npos) ? "/" : original.substr(slash);
    return t_scheme + "://" + t_host + rest;
}

std::string url_path(const std::string& url) {
    auto pos = url.find("://");
    if (pos == std::string::npos) return {};
    size_t p = pos + 3;
    size_t slash = url.find('/', p);
    if (slash == std::string::npos) return "/";
    size_t q = url.find('?', slash);
    return url.substr(slash, q == std::string::npos ? std::string::npos : q - slash);
}

void set_uwr_url(il2cpp::Il2CppObject* uwr, const std::string& url) {
    static il2cpp::Il2CppMethod* setter = nullptr;
    if (!setter) {
        il2cpp::Il2CppClass* k = il2cpp::find_class("UnityEngine.Networking", "UnityWebRequest");
        if (!k) return;
        setter = il2cpp::class_get_method_from_name(k, "set_url", 1);
        if (!setter) { MYO_LOG("http", "set_url not found"); return; }
    }
    il2cpp::Il2CppString* s = il2cpp::string_new(url.c_str());
    void* args[1] = { s };
    void* exc = nullptr;
    il2cpp::runtime_invoke(setter, uwr, args, &exc);
}

std::string get_uwr_url(il2cpp::Il2CppObject* uwr) {
    static il2cpp::Il2CppMethod* getter = nullptr;
    if (!getter) {
        il2cpp::Il2CppClass* k = il2cpp::find_class("UnityEngine.Networking", "UnityWebRequest");
        if (!k) return {};
        getter = il2cpp::class_get_method_from_name(k, "get_url", 0);
        if (!getter) { MYO_LOG("http", "get_url not found"); return {}; }
    }
    void* exc = nullptr;
    il2cpp::Il2CppString* s = static_cast<il2cpp::Il2CppString*>(
        il2cpp::runtime_invoke(getter, uwr, nullptr, &exc));
    return il2cpp::string_to_utf8(s);
}

extern "C" il2cpp::Il2CppObject* __cdecl myosotis_pre_send(il2cpp::Il2CppObject* self) {
    if (self) MYO_LOG("http", "SendWebRequest FIRED url={}", get_uwr_url(self));
    std::string url = get_uwr_url(self);
    if (!url.empty()) {
        std::string host, scheme;
        if (url_host_scheme(url, scheme, host)) {
            std::string server = narrow(myosotis::config::g.server);
            if (!server.empty()) {
                if (host == "notice.limbuscompanyapi.com") {
                    set_uwr_url(self, url_replace_host_scheme(url, server));
                }
                std::string path = url_path(url);
                if (path.rfind("/serverinfos_", 0) == 0) {
                    size_t extra = path.find('/', strlen("/serverinfos_"));
                    if (extra == std::string::npos) {
                        std::string si = narrow(myosotis::config::g.serverinfos_url);
                        if (!si.empty()) set_uwr_url(self, si);
                    }
                }
            }
        }
    }
    // Call the original SendWebRequest via the trampoline and return its result.
    // SendWebRequest returns an IEnumerator (coroutine); the caller needs it.
    if (g_send_original) {
        auto orig = reinterpret_cast<Send_t>(g_send_original);
        return orig(self, g_send_method);
    }
    return nullptr;
}


extern "C" il2cpp::Il2CppObject* __cdecl myosotis_post_prefix(il2cpp::Il2CppString* uri,
                                                            il2cpp::Il2CppString* body) {
    MYO_LOG("http", "Post FIRED uri={}", uri ? il2cpp::string_to_utf8(uri) : std::string("<null>"));
    std::string s_uri = il2cpp::string_to_utf8(uri);
    std::string server = narrow(myosotis::config::g.server);
    std::string new_uri = server.empty() ? s_uri : url_replace_host_scheme(s_uri, server);
    il2cpp::Il2CppString* rewritten = il2cpp::string_new(new_uri.c_str());
    if (g_post_original) {
        auto orig = reinterpret_cast<Post_t>(g_post_original);
        return orig(rewritten, body, nullptr);
    }
    return nullptr;
}

void install_one(const char* ns, const char* klass, const char* method,
                 int argc, void* stub) {
    il2cpp::Il2CppClass* k = il2cpp::find_class(ns, klass);
    if (!k) { MYO_LOG("http", "class {}.{} not found", ns, klass); return; }
    il2cpp::Il2CppMethod* m = il2cpp::class_get_method_from_name(k, method, argc);
    if (!m) { MYO_LOG("http", "method {}.{} /{} not found", klass, method, argc); return; }
    if (method == std::string("Post") && argc == 2) {
        g_post_method = m;
        // Read methodPointer (offset 0) before overwriting.
        g_post_original = *reinterpret_cast<void**>(m);
    }
    if (method == std::string("SendWebRequest") && argc == 0) {
        g_send_method = m;
        g_send_original = *reinterpret_cast<void**>(m);
    }
    // Inline hook: patches native code body AND overwrites methodPointer.
    void* old = myosotis::hook::install_inline(m, stub);
    if (old) {
        if (method == std::string("Post") && argc == 2) g_post_original = old;
        if (method == std::string("SendWebRequest") && argc == 0) g_send_original = old;
        MYO_LOG("http", "hooked {}.{} /{}", klass, method, argc);
    }
}

}  // namespace

bool install_http() {
    install_one("UnityEngine.Networking", "UnityWebRequest", "SendWebRequest", 0,
                reinterpret_cast<void*>(&myosotis_pre_send));
    install_one("UnityEngine.Networking", "UnityWebRequest", "Post", 2,
                reinterpret_cast<void*>(&myosotis_post_prefix));
    return true;
}

}  // namespace myosotis::patches
