#include "il2cpp_names.hpp"
// Login patch implementation.
#include "patches/login.hpp"
#include "il2cpp.hpp"
#include "config.hpp"
#include "log.hpp"
#include "hook.hpp"
#include <cstring>
#include <string>
#include <windows.h>

namespace myosotis::patches {

namespace {
// Find a field offset by name on a class hierarchy.
size_t find_field_offset(il2cpp::Il2CppClass* klass, const char* name) {
    for (il2cpp::Il2CppClass* k = klass; k; k = il2cpp::class_get_parent(k)) {
        void* iter = nullptr;
        while (auto* f = il2cpp::class_get_fields(k, &iter)) {
            const char* fn = il2cpp::field_get_name(f);
            if (fn && strcmp(fn, name) == 0) {
                size_t off = il2cpp::field_get_offset(f);
                const char* ns = il2cpp::class_get_namespace(k);
                MYO_LOG_DEBUG("login", "field {} at offset 0x{:x} on {}.{}", name, off, ns ? ns : "", il2cpp::class_get_name(k));
                return off;
            }
        }
    }
    MYO_LOG("login", "field {} not found on hierarchy", name);
    return 0;
}


// Narrow -> wide helper for config strings.
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == 0) s.pop_back();
    return s;
}

std::string g_token_utf8;  // cache of config.token as UTF-8

// --- SteamId hook: return a SteamId struct with Value = some ulong.
// The C# version returns SteamId { Value = randomULong }. SteamId.Value is the
// only field; we allocate a SteamId via il2cpp_object_new and set its Value
// field. We resolve the Value field offset at install time.
il2cpp::Il2CppObject* g_steamid = nullptr;
size_t g_steamid_value_off = 0;

extern "C" il2cpp::Il2CppObject* __cdecl myosotis_get_steamid() {
    if (g_steamid && g_steamid_value_off) {
        uint64_t v = 0x7700000000ULL;  // arbitrary stable steam-ish id
        *reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(g_steamid) + g_steamid_value_off) = v;
    }
    return g_steamid;
}

// --- GetAuthSessionTicket hook: return an AuthTicket with Data = bytes(token).
// AuthTicket.Data is a byte[]. We build an il2cpp byte array via il2cpp_array_new
// (we resolve it through il2cpp_array_class_get + il2cpp_array_new, or the
// dedicated il2cpp_array_new export). We cache the result so the same ticket
// is returned each call (matches C# returning a fresh AuthTicket each time,
// but the data is identical).
//
// AuthTicket layout (Steamworks.NET): single field `byte[] Data`. We resolve
// the Data field offset at install time.
il2cpp::Il2CppObject* g_authticket = nullptr;
size_t g_authticket_data_off = 0;

// We need il2cpp_array_new / il2cpp_array_class_get for building the byte[].
// Resolve them via GetProcAddress like the rest.
using pfn_array_class_get = il2cpp::Il2CppClass* (*)(il2cpp::Il2CppClass*, uint32_t);
using pfn_array_new        = il2cpp::Il2CppObject* (*)(il2cpp::Il2CppClass*, uintptr_t);
pfn_array_class_get g_array_class_get = nullptr;
pfn_array_new        g_array_new        = nullptr;

il2cpp::Il2CppObject* build_byte_array(const std::string& bytes) {
    if (!g_array_class_get || !g_array_new) return nullptr;
    // We need the byte class. Resolve via find_class("System", "Byte").
    il2cpp::Il2CppClass* byte_class = il2cpp::find_class("System", "Byte");
    if (!byte_class) return nullptr;
    il2cpp::Il2CppClass* arr_class = g_array_class_get(byte_class, 1);
    if (!arr_class) return nullptr;
    il2cpp::Il2CppObject* arr = g_array_new(arr_class, bytes.size());
    if (!arr) return nullptr;
    // il2cpp array layout: header + bounds + length (i32 at offset 0x10) + data at 0x20
    return arr;
}

extern "C" il2cpp::Il2CppObject* __cdecl myosotis_get_auth_ticket() {
    if (g_authticket && g_authticket_data_off) return g_authticket;
    // Build a byte[] from the token and store it on a new AuthTicket.
    if (g_token_utf8.empty()) return nullptr;
    // Resolve AuthTicket class + Data field.
    il2cpp::Il2CppClass* ticket_cls = il2cpp::find_class("Steamworks", "AuthTicket");
    if (!ticket_cls) ticket_cls = il2cpp::find_class("Steamworks", "AuthTicket_t");
    if (!ticket_cls) return nullptr;
    il2cpp::Il2CppObject* ticket = static_cast<il2cpp::Il2CppObject*>(il2cpp::object_new(ticket_cls));
    // Resolve the Data field by name on the AuthTicket class hierarchy.
    g_authticket_data_off = find_field_offset(ticket_cls, "Data");
    if (!g_authticket_data_off) g_authticket_data_off = 0x10;
    il2cpp::Il2CppObject* data_arr = build_byte_array(g_token_utf8);
    if (!data_arr) return nullptr;
    *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(ticket) + g_authticket_data_off) = data_arr;
    g_authticket = ticket;
    return g_authticket;
}

// --- SteamClient.Init hook: no-op, return true.
extern "C" bool __cdecl myosotis_steam_init() { return true; }

// --- LoginInfoManager.ProviderLogin_Steam / StartSendPresence / StopSendPresence: void no-ops.
extern "C" void __cdecl myosotis_login_noop() {}

// --- PlayerPrefs.GetInt(key, def): return 1 (GUEST) for account-ish keys.
// The C# version inspects the key string. We do the same by reading the key
// argument (an il2cpp string).
extern "C" int32_t __cdecl myosotis_playerprefs_getint(il2cpp::Il2CppString* key, int32_t def) {
    MYO_LOG_DEBUG("login", "PlayerPrefs.GetInt FIRED key={}", key ? il2cpp::string_to_utf8(key) : std::string("<null>"));
    if (key) {
        std::string k = il2cpp::string_to_utf8(key);
        const char* needles[] = { "Account", "account", "Guest", "guest", "Login", "login" };
        for (auto n : needles) if (strstr(k.c_str(), n)) return 1;  // ACCOUNT_TYPE.GUEST
    }
    return def;
}

// Resolve a GameAssembly export we need but didn't put in the bridge.
void* resolve_extra(const char* canonical) {
    const std::string& obf = myosotis::il2cpp_names::lookup(canonical);  // <-- needs the header
    // (We include il2cpp_names.hpp below.)
    if (obf.empty()) return nullptr;
    HMODULE ga = GetModuleHandleW(L"GameAssembly.dll");
    return ga ? reinterpret_cast<void*>(GetProcAddress(ga, obf.c_str())) : nullptr;
}

void install_one(const char* ns, const char* klass, const char* method, void* stub) {
    il2cpp::Il2CppClass* k = il2cpp::find_class(ns, klass);
    if (!k) { MYO_LOG("login", "class {}.{} not found", ns, klass); return; }
    // Try common arg counts.
    il2cpp::Il2CppMethod* m = nullptr;
    for (int argc = 0; argc <= 2 && !m; ++argc)
        m = il2cpp::class_get_method_from_name(k, method, argc);
    if (!m) { MYO_LOG("login", "method {}.{} not found", klass, method); return; }
    if (myosotis::hook::install_inline(m, stub)) {
        MYO_LOG("login", "hooked {}.{}", klass, method);
    }
}

}  // namespace

bool install_login() {
    g_token_utf8 = narrow(myosotis::config::g.token);
    if (g_token_utf8.empty()) {
        MYO_LOG("login", "WARNING: no token set in myosotis.ini; auth will fail");
    } else {
        MYO_LOG("login", "token loaded ({} bytes)", g_token_utf8.size());
    }

    // Resolve the array helpers we need for building byte[].
    g_array_class_get = reinterpret_cast<pfn_array_class_get>(resolve_extra("il2cpp_array_class_get"));
    g_array_new        = reinterpret_cast<pfn_array_new>(resolve_extra("il2cpp_array_new"));
    if (!g_array_class_get || !g_array_new) {
        MYO_LOG("login", "array helpers missing (class_get={} new={})",
                 reinterpret_cast<void*>(g_array_class_get), reinterpret_cast<void*>(g_array_new));
    }

    // Pre-build the SteamId object so the hook just mutates its Value field.
    if (il2cpp::Il2CppClass* sid = il2cpp::find_class("Steamworks", "SteamId")) {
        g_steamid = static_cast<il2cpp::Il2CppObject*>(il2cpp::object_new(sid));
        g_steamid_value_off = find_field_offset(sid, "Value");
        if (!g_steamid_value_off) g_steamid_value_off = 0x10;
    } else {
        MYO_LOG("login", "Steamworks.SteamId not found");
    }
    install_one("Steamworks", "SteamClient", "Init", reinterpret_cast<void*>(&myosotis_steam_init));
    install_one("Steamworks", "ISteamUser", "GetSteamID", reinterpret_cast<void*>(&myosotis_get_steamid));
    install_one("Steamworks", "SteamUser", "GetAuthSessionTicket", reinterpret_cast<void*>(&myosotis_get_auth_ticket));
    install_one("Login", "LoginInfoManager", "ProviderLogin_Steam", reinterpret_cast<void*>(&myosotis_login_noop));
    install_one("Login", "LoginInfoManager", "StartSendPresence", reinterpret_cast<void*>(&myosotis_login_noop));
    install_one("Login", "LoginInfoManager", "StopSendPresence", reinterpret_cast<void*>(&myosotis_login_noop));
    install_one("UnityEngine", "PlayerPrefs", "GetInt", reinterpret_cast<void*>(&myosotis_playerprefs_getint));
    return true;
}

}  // namespace myosotis::patches
