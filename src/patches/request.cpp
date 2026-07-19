// Request patch implementation.
#include "patches/request.hpp"
#include "il2cpp.hpp"
#include "il2cpp_names.hpp"
#include "config.hpp"
#include "http.hpp"
#include "log.hpp"
#include "hook.hpp"
#include <windows.h>
#include <string>
#include <unordered_map>
#include <cstring>
#include <cstdio>

namespace myosotis::patches {

namespace {

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

// apiPath -> packetId. Key is the absolute path (e.g. "/player/login").
std::unordered_map<std::string, int64_t> g_packet_ids;

// --- Invoke a parameterless instance method that returns a managed string.
// Returns UTF-8 string or empty on failure.
std::string invoke_string_method(il2cpp::Il2CppObject* obj, il2cpp::Il2CppMethod* m) {
    if (!obj || !m) return {};
    void* exc = nullptr;
    il2cpp::Il2CppString* s = static_cast<il2cpp::Il2CppString*>(
        il2cpp::runtime_invoke(m, obj, nullptr, &exc));
    return il2cpp::string_to_utf8(s);
}

// Invoke a parameterless instance method that returns an int (boxed or unboxed).
// get_PacketId returns an int. il2cpp_runtime_invoke boxes the return for value
// types, so we unbox: read the int at the boxed object's data area (offset 0x10).
int64_t invoke_int_method(il2cpp::Il2CppObject* obj, il2cpp::Il2CppMethod* m) {
    if (!obj || !m) return -1;
    void* exc = nullptr;
    void* ret = il2cpp::runtime_invoke(m, obj, nullptr, &exc);
    if (!ret) return -1;
    // Boxed int: header (0x10) + int32 at offset 0x10.
    return static_cast<int64_t>(*reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(ret) + 0x10));
}



// Build the apiPath for a Command type, mirroring GetApiPath in Request.cs:
//   apiClass = invoke the virtual get_ApiClass (returns an enum)
//   apiPath  = invoke the virtual get_ApiPath (returns a string)
//   result   = "/" + apiClass.ToLowerInvariant() + apiPath
//
// We don't have reflection, so we resolve the two methods by name. The C#
// version identifies them by signature (virtual, return type string / enum).
// In il2cpp we walk the class's methods and match by return type kind +
// instance + 0 params.
bool build_api_path(il2cpp::Il2CppClass* klass, std::string& out) {
    // The C# version finds apiPath/apiClass on type.BaseType.GetMethods() —
    // i.e. the HttpApiSchema<TReq,TResp> base, not the concrete Command.
    // class_get_methods in il2cpp returns only declared methods, so we must
    // walk the parent chain ourselves.
    il2cpp::Il2CppMethod* m_str = nullptr;
    il2cpp::Il2CppMethod* m_enum = nullptr;
    for (il2cpp::Il2CppClass* k = klass; k && (!m_str || !m_enum); k = il2cpp::class_get_parent(k)) {
        void* iter = nullptr;
        while (auto* m = il2cpp::class_get_methods(k, &iter)) {
            if (il2cpp::method_get_param_count(m) != 0) continue;
            if (!il2cpp::method_is_instance(m)) continue;
            il2cpp::Il2CppType* rt = il2cpp::method_get_return_type(m);
            if (!rt) continue;
            int tk = il2cpp::type_get_type(rt);
            if (tk == 0x0E && !m_str) m_str = m;            // STRING
            // il2cpp encodes enums as VALUETYPE (0x11) — the C# IsEnum check
            // is class_is_enum on the return type's class. Also accept 0x55
            // (ENUM in the Mono encoding) for safety.
            else if (tk == 0x11 /*VALUETYPE*/ || tk == 0x55 /*ENUM*/) {
                il2cpp::Il2CppClass* rc = il2cpp::type_get_class_or_element_class(rt);
                if (rc && il2cpp::class_is_enum(rc) && !m_enum) m_enum = m;
            }
        }
    }

    // One-shot diagnostic: dump every 0-param instance method on the parent
    // chain of the first schema class we see, so we can find the enum-returning
    // getter the C# version keys on (ReturnType.IsEnum).
    static bool dumped = false;
    if (!dumped) {
        dumped = true;
        MYO_LOG("request", "=== method dump for {} (parent chain) ===", il2cpp::class_get_name(klass));
        for (il2cpp::Il2CppClass* k = klass; k; k = il2cpp::class_get_parent(k)) {
            const char* kn = il2cpp::class_get_name(k);
            if (!kn) break;
            MYO_LOG("request", "  class {}", kn);
            void* it = nullptr;
            while (auto* m = il2cpp::class_get_methods(k, &it)) {
                if (il2cpp::method_get_param_count(m) != 0) continue;
                if (!il2cpp::method_is_instance(m)) continue;
                il2cpp::Il2CppType* rt = il2cpp::method_get_return_type(m);
                int tk = rt ? il2cpp::type_get_type(rt) : -1;
                il2cpp::Il2CppClass* rc = rt ? il2cpp::type_get_class_or_element_class(rt) : nullptr;
                bool isenum = rc && il2cpp::class_is_enum(rc);
                MYO_LOG("request", "    {} ret_type=0x{:x} is_enum={}",
                         il2cpp::method_get_name(m), tk, isenum ? 1 : 0);
            }
            if (strcmp(kn, "Object") == 0) break;
        }
    }

    if (!m_str || !m_enum) {
        MYO_LOG("request", "build_api_path: {} missing m_str={} m_enum={}",
                 il2cpp::class_get_name(klass),
                 m_str ? il2cpp::method_get_name(m_str) : "<null>",
                 m_enum ? il2cpp::method_get_name(m_enum) : "<null>");
        return false;
    }
    il2cpp::Il2CppObject* inst = static_cast<il2cpp::Il2CppObject*>(il2cpp::object_new(klass));
    if (!inst) return false;
    il2cpp::runtime_class_init(klass);
    std::string path = invoke_string_method(inst, m_str);

    // Resolve the apiClass enum name: invoke the enum-returning method on the
    // instance, then call ToString() on the boxed enum to get the member name.
    std::string enum_name;
    void* exc = nullptr;
    il2cpp::Il2CppObject* enum_obj = static_cast<il2cpp::Il2CppObject*>(
        il2cpp::runtime_invoke(m_enum, inst, nullptr, &exc));
    if (enum_obj && !exc) {
        // Find ToString() /0 on the enum's runtime class.
        il2cpp::Il2CppClass* ec = il2cpp::object_get_class(enum_obj);
        if (ec) {
            il2cpp::Il2CppMethod* ts = il2cpp::class_get_method_from_name(ec, "ToString", 0);
            if (ts) {
                void* exc2 = nullptr;
                il2cpp::Il2CppString* es = static_cast<il2cpp::Il2CppString*>(
                    il2cpp::runtime_invoke(ts, enum_obj, nullptr, &exc2));
                if (es && !exc2) enum_name = il2cpp::string_to_utf8(es);
            }
        }
    }

    // Lowercase the enum name and prepend as /<enumname><path>.
    std::string lower_enum = enum_name;
    for (char& c : lower_enum) { if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a'); }

    std::string full = "/" + lower_enum + (path.empty() ? std::string() :
                       (path[0] == '/' ? path : std::string("/") + path));
    out = full;
    MYO_LOG("request", "api_path for {} = {} (enum={})", il2cpp::class_get_name(klass), out, enum_name);
    return !out.empty();
}

// Scan Assembly-CSharp for *Command types whose base is HttpApiSchema<TReq,TResp>,
// and build the packetId map.
void build_packet_id_map() {
    size_t count = 0;
    il2cpp::Il2CppAssembly** asms = il2cpp::domain_get_assemblies(&count);
    if (!asms) return;
    [[maybe_unused]] il2cpp::Il2CppClass* schema_base = nullptr;
    int cmd_count = 0, schema_count = 0;
    for (size_t i = 0; i < count; ++i) {
        il2cpp::Il2CppImage* img = il2cpp::assembly_get_image(asms[i]);
        if (!img) continue;
        const char* name = il2cpp::image_get_name(img);
        if (!name) continue;
        if (!strstr(name, "Assembly-CSharp")) continue;
        size_t cn = il2cpp::image_get_class_count(img);
        for (size_t c = 0; c < cn; ++c) {
            il2cpp::Il2CppClass* klass = il2cpp::image_get_class(img, c);
            if (!klass) continue;
            const char* kn = il2cpp::class_get_name(klass);
            if (!kn) continue;
            // Name ends with "Command"
            size_t knl = strlen(kn);
            if (knl < 7 || strcmp(kn + knl - 7, "Command") != 0) continue;
            ++cmd_count;
            // C# filter: type.BaseType.GenericTypeArguments.Length == 2.
            // The base must be a constructed generic with 2 type args. In il2cpp,
            // a constructed generic class's name carries the arity backtick
            // (e.g. "HttpApiSchema`2"). We check the IMMEDIATE parent only
            // (BaseType, not the whole chain) and require the backtick + "2".
            il2cpp::Il2CppClass* p = il2cpp::class_get_parent(klass);
            bool is_schema = false;
            if (p) {
                const char* pn = il2cpp::class_get_name(p);
                if (cmd_count <= 5)
                    MYO_LOG("request", "cmd {} {}: parent = {}", cmd_count, kn, pn ? pn : "<null>");
                // Constructed generic with arity 2: name ends with "`2".
                if (pn) {
                    size_t pnl = strlen(pn);
                    if (pnl >= 2 && pn[pnl-2] == '`' && pn[pnl-1] == '2') {
                        is_schema = true; schema_base = p;
                    }
                }
            }
            if (!is_schema) continue;
            ++schema_count;

            // Build apiPath.
            std::string api_path;
            if (!build_api_path(klass, api_path)) {
                if (schema_count <= 3) MYO_LOG("request", "build_api_path failed for {}", kn);
                continue;
            }
            if (schema_count <= 3) MYO_LOG("request", "api_path for {} = {}", kn, api_path);

            // C# uses type.BaseType.GenericTypeArguments[1] (TResp) and
            // invokes get_PacketId on an instance of TResp. We resolve TResp
            // by reading the parent's Il2CppType generic_class metadata.
            // Il2CppType layout: {type bitfield @0, data union @8}.
            // For GENERICINST (0x21), data.generic_class -> Il2CppGenericClass:
            //   type @0, context.class_inst @8.
            // Il2CppGenericInst: { type_argc @0, type_argv @8 }.
            il2cpp::Il2CppClass* resp_class = nullptr;
            il2cpp::Il2CppClass* parent = il2cpp::class_get_parent(klass);
            if (parent) {
                il2cpp::Il2CppType* parent_type = il2cpp::class_get_type(parent);
                if (parent_type) {
                    void* generic_class = *reinterpret_cast<void**>(
                        reinterpret_cast<uintptr_t>(parent_type));
                    if (generic_class) {
                        // context.class_inst is at offset 8
                        void* class_inst = *reinterpret_cast<void**>(
                            reinterpret_cast<uintptr_t>(generic_class) + 8);
                        if (class_inst) {
                            // type_argc @0, type_argv @8
                            uint32_t argc = *reinterpret_cast<uint32_t*>(class_inst);
                            void** argv = reinterpret_cast<void**>(
                                *reinterpret_cast<uintptr_t*>(
                                    reinterpret_cast<uintptr_t>(class_inst) + 8));
                            if (argc >= 2 && argv) {
                                il2cpp::Il2CppType* resp_type =
                                    static_cast<il2cpp::Il2CppType*>(argv[1]);
                                resp_class = il2cpp::class_from_type(resp_type);
                            }
                        }
                    }
                }
            }
            if (!resp_class) {
                if (schema_count <= 3) MYO_LOG("request", "could not resolve TResp for {}", kn);
                continue;
            }
            if (schema_count <= 3) MYO_LOG("request", "TResp for {} = {}", kn, il2cpp::class_get_name(resp_class));

            // Find get_PacketId on the response type (TResp).
            il2cpp::Il2CppMethod* pid = nullptr;
            for (il2cpp::Il2CppClass* rc = resp_class; rc && !pid; rc = il2cpp::class_get_parent(rc)) {
                void* iter2 = nullptr;
                while (auto* m = il2cpp::class_get_methods(rc, &iter2)) {
                    if (il2cpp::method_get_param_count(m) != 0) continue;
                    if (!il2cpp::method_is_instance(m)) continue;
                    if (strcmp(il2cpp::method_get_name(m), "get_PacketId") == 0) { pid = m; break; }
                }
            }
            if (!pid) {
                if (schema_count <= 3) MYO_LOG("request", "get_PacketId not found on TResp {} for {}", il2cpp::class_get_name(resp_class), kn);
                continue;
            }
            il2cpp::Il2CppObject* inst = static_cast<il2cpp::Il2CppObject*>(il2cpp::object_new(resp_class));
            if (!inst) {
                if (schema_count <= 3) MYO_LOG("request", "object_new failed for {}", kn);
                continue;
            }
            il2cpp::runtime_class_init(resp_class);
            int64_t pid_val = invoke_int_method(inst, pid);
            if (pid_val < 0) {
                if (schema_count <= 3) MYO_LOG("request", "get_PacketId invoke failed for {} (val={})", kn, pid_val);
                continue;
            }
            g_packet_ids[api_path] = pid_val;
            MYO_LOG("request", "packetId[{}] = {}", api_path, pid_val);
        }
    }
    MYO_LOG("request", "packet-id map size: {} (cmds={}, schemas={})",
             g_packet_ids.size(), cmd_count, schema_count);
}

// --- AddRequest hook.
// Signature: HttpApiRequester.AddRequest(HttpApiRequester this, HttpApiSchema schema, MethodInfo*).
// We invoke the schema's URL + body getters (found by signature), POST via WinHTTP,
// rewrite packetId, invoke the schema's _responseEvent.Invoke(text).

// Field offset cache for HttpApiSchema._responseEvent. We resolve it by walking
// the schema class's fields looking for one of type UnityEvent<string> (we just
// take the first field whose type name contains "UnityEvent").
size_t g_response_event_off = 0;
il2cpp::Il2CppClass* g_schema_class = nullptr;


// Per-schema URL/body method cache (keyed by schema class).
struct SchemaMethods {
    il2cpp::Il2CppMethod* url = nullptr;
    il2cpp::Il2CppMethod* body = nullptr;
    bool scanned = false;
};
std::unordered_map<il2cpp::Il2CppClass*, SchemaMethods> g_schema_methods;

SchemaMethods resolve_schema_methods(il2cpp::Il2CppClass* klass) {
    SchemaMethods out;
    void* iter = nullptr;
    while (auto* m = il2cpp::class_get_methods(klass, &iter)) {
        if (il2cpp::method_get_param_count(m) != 0) continue;
        if (!il2cpp::method_is_instance(m)) continue;
        il2cpp::Il2CppType* rt = il2cpp::method_get_return_type(m);
        if (!rt) continue;
        if (il2cpp::type_get_type(rt) != 0x0E) continue;  // STRING
        // Probe by invoking on a temp instance and checking the result prefix.
        // That's expensive per-method; instead we use a heuristic: the URL method
        // returns something starting with "http", the body with "{". We can't
        // invoke without an instance; defer the disambiguation to call time by
        // caching both string-returning 0-arg instance methods and picking at
        // invoke time. For now, record both candidates.
        if (!out.url) out.url = m;       // first string method
        else if (!out.body) out.body = m; // second string method
    }
    out.scanned = true;
    return out;
}

// Find _responseEvent field offset on the schema's class hierarchy.
size_t find_response_event_off(il2cpp::Il2CppClass* klass) {
    if (g_response_event_off) return g_response_event_off;
    for (il2cpp::Il2CppClass* k = klass; k; k = il2cpp::class_get_parent(k)) {
        void* iter = nullptr;
        while (auto* f = il2cpp::class_get_fields(k, &iter)) {
            const char* fname = il2cpp::field_get_name(f);
            if (fname && strcmp(fname, "_responseEvent") == 0) {
                g_response_event_off = il2cpp::field_get_offset(f);
                const char* ns = il2cpp::class_get_namespace(k);
                MYO_LOG("request", "found _responseEvent at offset 0x{:x} on {}.{}",
                        g_response_event_off, ns ? ns : "", il2cpp::class_get_name(k));
                return g_response_event_off;
            }
        }
    }
    MYO_LOG("request", "_responseEvent field not found on hierarchy; using 0x18 fallback");
    g_response_event_off = 0x18;
    return g_response_event_off;
}

extern "C" void __cdecl myosotis_add_request(il2cpp::Il2CppObject* self, il2cpp::Il2CppObject* schema) {
    MYO_LOG("request", "AddRequest hook FIRED (self={} schema={})", static_cast<void*>(self), static_cast<void*>(schema));
    (void)self;  // HttpApiRequester instance; we don't need it for the POST.
    if (!schema) return;
    il2cpp::Il2CppClass* klass = il2cpp::find_class("Server", "HttpApiSchema");
    // We don't strictly need the declared class; use the schema's runtime type.
    // il2cpp_object_get_class would give us that, but we didn't wrap it. Use the
    // schema's vtable/first field heuristically: assume `schema` is a HttpApiSchema
    // instance. Walk methods on the *static* HttpApiSchema class for URL/body.
    if (!klass) klass = il2cpp::find_class("HttpApiRequester", "HttpApiSchema");
    if (!klass) return;
    if (!g_schema_class) g_schema_class = klass;

    auto& sm = g_schema_methods[klass];
    if (!sm.scanned) sm = resolve_schema_methods(klass);

    // Build a temporary instance to invoke the URL/body getters (the C# version
    // invokes them on the passed `schema`). We invoke on `schema` directly.
    std::string url  = invoke_string_method(schema, sm.url);
    std::string body = invoke_string_method(schema, sm.body);
    if (url.empty() || body.empty()) {
        MYO_LOG("request", "could not resolve url/body for schema {}", static_cast<void*>(schema));
        return;
    }

    std::string server = narrow(myosotis::config::g.server);
    std::string final_url = server.empty() ? url : url_replace_host_scheme(url, server);

    std::string path = url_path(final_url);

    // Look up the correct packetId from our static map (path → packetId).
    auto it = g_packet_ids.find(path);
    std::string map_pid = (it != g_packet_ids.end()) ? std::to_string(it->second) : std::string{};

    // Replace the packetId in the request body with the map's value so the
    // server receives the correct one. The server echoes it back, so no
    // response rewriting is needed.
    std::string body_out = body;
    if (!map_pid.empty()) {
        const char* needle = "\"packetId\":";
        size_t p = body_out.find(needle);
        if (p != std::string::npos) {
            p += strlen(needle);
            while (p < body_out.size() && (body_out[p] == ' ' || body_out[p] == '\t')) ++p;
            size_t digits_start = p;
            while (p < body_out.size() && body_out[p] >= '0' && body_out[p] <= '9') ++p;
            if (p > digits_start) {
                body_out.replace(digits_start, p - digits_start, map_pid);
            }
        }
    }
    MYO_LOG("request", "POST {} (path={} pid={} body_len={})", final_url, path, map_pid.empty() ? "none" : map_pid, body_out.size());

    myosotis::http::Response r = myosotis::http::post(final_url, body_out, map_pid);
    if (r.status == 0) {
        MYO_LOG("request", "POST {} failed: {}", final_url, r.error);
        return;
    }
    MYO_LOG("request", "response status={} body={}", r.status, r.body);
    std::string text = std::move(r.body);

    // Invoke schema._responseEvent.Invoke(text).
    size_t off = find_response_event_off(g_schema_class);
    void* evt = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(schema) + off);
    if (!evt) { MYO_LOG("request", "responseEvent is null at off 0x{:x}", off); return; }

    // Resolve Invoke(string) on the actual runtime class of the event object.
    // Cache it — the event type is the same (DelegateEventString) for every
    // schema instance.
    static il2cpp::Il2CppMethod* invoke_method = nullptr;
    if (!invoke_method) {
        il2cpp::Il2CppClass* ue = il2cpp::object_get_class(evt);
        if (ue) {
            void* it2 = nullptr;
            while (auto* m = il2cpp::class_get_methods(ue, &it2)) {
                const char* mn = il2cpp::method_get_name(m);
                uint32_t pc = il2cpp::method_get_param_count(m);
                if (mn && strcmp(mn, "Invoke") == 0 && pc == 1) {
                    invoke_method = m;
                    const char* ns = il2cpp::class_get_namespace(ue);
                    MYO_LOG("request", "cached Invoke /1 on {}.{}", ns ? ns : "", il2cpp::class_get_name(ue));
                    break;
                }
            }
        }
    }
    if (!invoke_method) { MYO_LOG("request", "UnityEvent.Invoke not resolved"); return; }
    il2cpp::Il2CppString* s = il2cpp::string_new(text.c_str());
    void* args[1] = { s };
    void* exc = nullptr;
    il2cpp::runtime_invoke(invoke_method, evt, args, &exc);
    if (exc) MYO_LOG("request", "_responseEvent.Invoke threw (exc={})", static_cast<void*>(exc));
}

// Log every method named `method` on `klass` (all overloads), so we can see
// which arg counts exist. This is diagnostic only.
void log_method_overloads(il2cpp::Il2CppClass* k, const char* method) {
    void* iter = nullptr;
    while (auto* m = il2cpp::class_get_methods(k, &iter)) {
        const char* nm = il2cpp::method_get_name(m);
        if (!nm || strcmp(nm, method) != 0) continue;
        MYO_LOG("request", "  {} /{} params instance={}",
                nm, il2cpp::method_get_param_count(m),
                il2cpp::method_is_instance(m) ? 1 : 0);
    }
}

void install_one(const char* ns, const char* klass, const char* method,
                 void* stub) {
    il2cpp::Il2CppClass* k = il2cpp::find_class(ns, klass);
    if (!k) { MYO_LOG("request", "class {}.{} not found", ns, klass); return; }
    // Dump every overload so we can see what the runtime exposes.
    MYO_LOG("request", "overloads of {}.{}:", klass, method);
    log_method_overloads(k, method);
    // Hook EVERY method with this name (all overloads) — we don't know which
    // the game calls, and methodPointer overwrite on the wrong overload is
    // harmless (the game just doesn't hit it). Hooking all is safest.
    int hooked = 0;
    void* iter = nullptr;
    while (auto* m = il2cpp::class_get_methods(k, &iter)) {
        const char* nm = il2cpp::method_get_name(m);
        if (!nm || strcmp(nm, method) != 0) continue;
        // Use inline hook (patches native code body) since methodPointer
        // overwrite alone doesn't intercept direct managed-to-managed calls.
        void* old = myosotis::hook::install_inline(m, stub);
        if (old) {
            MYO_LOG("request", "hooked {}.{} /{}", klass, method, il2cpp::method_get_param_count(m));
            ++hooked;
        }
    }
    if (hooked == 0) MYO_LOG("request", "no overloads of {}.{} found to hook", klass, method);
}
}  // namespace (anonymous)

bool install_request() {
    build_packet_id_map();
    install_one("Server", "HttpApiRequester", "AddRequest",
                reinterpret_cast<void*>(&myosotis_add_request));
    return true;
}


}  // namespace myosotis::patches
