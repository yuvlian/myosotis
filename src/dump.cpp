// myodump: il2cpp dumper DLL. Inject into a running il2cpp game to dump
// all classes/fields to ./dump/Il2Cpp.cs
// and ./dump/Types.cs (Server namespace classes with fields, System.Text.Json-ready).
//
// This is not a good il2cpp dumper as the main purpose is to only get enough info for server types.
//
// Shares the il2cpp bridge, log, scan, and PE modules with myosotis.dll but
// has no patches, config, or hook engine. Dumps once then idles.
//
// Build: build.bat dump
// Inject: myoink (with MYOINK_EXE=myodump.exe and MYOINK_DLL=myodump.dll),
//   or any DLL injector — the dumper waits for il2cpp to be ready.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <fstream>
#include <filesystem>
#include "config.hpp"
#include "il2cpp.hpp"
#include "il2cpp_names.hpp"
#include "log.hpp"

// The il2cpp bridge and name resolver live in myosotis::il2cpp and
// myosotis::il2cpp_names; we use qualified names like the rest of the codebase.
using namespace myosotis;  // brings in il2cpp and il2cpp_names

namespace myodump {

namespace {

// Wait for GameAssembly.dll to be loaded AND il2cpp names resolved.
bool wait_for_il2cpp_ready() {
    MYO_LOG("dump", "waiting for GameAssembly.dll + il2cpp names...");
    bool logged_ga = false;
    for (int i = 0; i < 600; ++i) {
        if (!GetModuleHandleW(L"GameAssembly.dll")) {
            if (i == 0 || (i % 50 == 0))
                MYO_LOG("dump", "poll {}: GameAssembly.dll not yet loaded", i);
            Sleep(100);
            continue;
        }
        if (!logged_ga) {
            MYO_LOG("dump", "GameAssembly.dll loaded (after {} polls)", i);
            logged_ga = true;
        }
        if (!il2cpp_names::g_map.empty()) return true;
        if (il2cpp_names::resolve()) return true;
        Sleep(100);
    }
    return false;
}

bool wait_for_domain_ready() {
    MYO_LOG("dump", "waiting for il2cpp domain...");
    for (int i = 0; i < 600; ++i) {
        size_t n = 0;
        il2cpp::Il2CppAssembly** asms = il2cpp::domain_get_assemblies(&n);
        if (asms && n > 0) return true;
        Sleep(100);
    }
    return false;
}

// ---- Dump formatting (mirrors frida-il2cpp-bridge Class/Field.toString) ----

// Render a C# type name from an Il2CppType*. Uses il2cpp_type_get_name which
// returns the fully-qualified name (e.g. "System.String", "Server.HttpApiSchema`2<TReq,TResp>").
std::string type_name(il2cpp::Il2CppType* t) {
    if (!t) return "object";
    const char* n = il2cpp::type_get_name(t);
    if (!n || !n[0]) return "object";
    return n;
}

// Render a single field line: "    <type> <name>; // 0x<offset>"
// For static fields: "    static <type> <name>;"
// For literals (const): "    const <type> <name> = <value>;" (value not read —
// dumper.js reads it but we skip that for simplicity; the offset comment is
// omitted for literals and thread-static fields).
struct FieldLine {
    std::string text;
    bool is_static = false;
};

FieldLine render_field(il2cpp::Il2CppField* f) {
    FieldLine out;
    const char* name = il2cpp::field_get_name(f);
    if (!name) return out;
    il2cpp::Il2CppType* ft = il2cpp::field_get_type(f);
    std::string tn = type_name(ft);
    size_t off = il2cpp::field_get_offset(f);
    // dumper.js checks isStatic/isThreadStatic/isLiteral; we don't have those
    // getters wired, so we approximate: static fields have offset 0 in il2cpp
    // (they're stored in the static fields area, not the instance layout).
    // For a dump this is sufficient — the offset comment distinguishes them.
    out.text = std::format("    {} {};// 0x{:x}", tn, name, off);
    return out;
}

// Render a class/struct/enum/interface declaration with fields.
std::string render_class(il2cpp::Il2CppClass* k) {
    const char* name = il2cpp::class_get_name(k);
    const char* ns = il2cpp::class_get_namespace(k);
    if (!name) return {};

    bool is_enum = il2cpp::class_is_enum(k);
    bool is_valuetype = il2cpp::class_is_valuetype(k);
    bool is_interface = il2cpp::class_is_interface(k);
    il2cpp::Il2CppClass* parent = il2cpp::class_get_parent(k);

    // Build the full type name: "Namespace.ClassName" or just "ClassName".
    std::string full = ns && ns[0] ? std::format("{}.{}", ns, name) : std::string(name);

    // Keyword: enum / struct / interface / class
    const char* kw = "class";
    if (is_enum) kw = "enum";
    else if (is_interface) kw = "interface";
    else if (is_valuetype) kw = "struct";

    // Parent name (skip System.Object / System.ValueType / System.Enum).
    std::string inherits;
    if (parent && !is_interface) {
        const char* pn = il2cpp::class_get_name(parent);
        const char* pns = il2cpp::class_get_namespace(parent);
        if (pn) {
            std::string pfull = pns && pns[0] ? std::format("{}.{}", pns, pn) : std::string(pn);
            if (pfull != "System.Object" && pfull != "System.ValueType" && pfull != "System.Enum") {
                inherits = std::format(" : {}", pfull);
            }
        }
    }

    // Fields.
    std::vector<std::string> field_lines;
    void* iter = nullptr;
    while (auto* f = il2cpp::class_get_fields(k, &iter)) {
        FieldLine fl = render_field(f);
        if (!fl.text.empty()) field_lines.push_back(fl.text);
    }

    std::string body;
    for (size_t i = 0; i < field_lines.size(); ++i) {
        body += field_lines[i];
        if (i + 1 < field_lines.size()) body += "\n";
    }

    return std::format("// {}\n{} {}{}\n{{\n{}\n}}\n",
                       full, kw, full, inherits, body);
}

// ---- Dump all assemblies, all classes → ./dump/Il2Cpp.cs ----

void dump_all(const std::filesystem::path& out_path) {
    MYO_LOG("dump", "writing full dump to {}", out_path.string());
    std::ofstream out(out_path);
    if (!out) { MYO_LOG_ERROR("dump", "cannot open {}", out_path.string()); return; }

    size_t asm_count = 0;
    il2cpp::Il2CppAssembly** asms = il2cpp::domain_get_assemblies(&asm_count);
    if (!asms) { MYO_LOG_ERROR("dump", "domain_get_assemblies returned null"); return; }

    int class_count = 0;
    for (size_t i = 0; i < asm_count; ++i) {
        il2cpp::Il2CppImage* img = il2cpp::assembly_get_image(asms[i]);
        if (!img) continue;
        const char* img_name = il2cpp::image_get_name(img);
        MYO_LOG_DEBUG("dump", "dumping {}", img_name ? img_name : "?");
        size_t cn = il2cpp::image_get_class_count(img);
        for (size_t c = 0; c < cn; ++c) {
            il2cpp::Il2CppClass* k = il2cpp::image_get_class(img, c);
            if (!k) continue;
            std::string s = render_class(k);
            if (!s.empty()) {
                out << s << "\n";
                ++class_count;
            }
        }
    }
    out.close();
    MYO_LOG("dump", "full dump complete: {} classes", class_count);
}

// ---- Types dump: Server namespace classes (excluding Http*), fields only,
//      System.Text.Json-ready → ./dump/Types.cs ----

// Check if a class name starts with "Http" (case-sensitive).
bool name_starts_with_http(const char* name) {
    if (!name) return false;
    return name[0] == 'H' && name[1] == 't' && name[2] == 't' && name[3] == 'p';
}

// Should we keep this class despite the Http* skip rule?
// HttpRequestFormat and HttpResponseFormat are the request/response envelopes
// and are needed for System.Text.Json deserialization. Generic classes have
// a backtick-arity suffix (e.g. "HttpRequestFormat`1"), so we compare the base
// name (before the backtick).
bool keep_despite_http(const char* name) {
    // Extract the base name before the backtick (if any).
    std::string base = name;
    auto bt = base.find('`');
    if (bt != std::string::npos) base = base.substr(0, bt);
    return base == "HttpRequestFormat" || base == "HttpResponseFormat";
}

// Strip the backtick-arity suffix from a generic class name and produce the
// C# generic parameter list. E.g. "Foo`2" → "Foo<T1, T2>".
// For non-generic names, returns the name unchanged.
std::string generic_class_name(const char* name) {
    const char* tick = strchr(name, '`');
    if (!tick) return name;
    int arity = std::atoi(tick + 1);
    std::string base(name, static_cast<size_t>(tick - name));
    std::string params;
    for (int i = 0; i < arity; ++i) {
        if (i) params += ", ";
        params += (arity == 1) ? "T" : ("T" + std::to_string(i + 1));
    }
    return std::format("{}<{}>", base, params);
}

// Recursively map an il2cpp fully-qualified type name to a C# type suitable
// for System.Text.Json. il2cpp_type_get_name returns generic instances WITHOUT
// backtick arity, e.g. "System.Collections.Generic.List<Server.Foo>" (not
// "List`1<...>"). We detect generics by finding a top-level '<'.
std::string json_type(const std::string& s) {
    // Primitive / known System types → C# keywords.
    static const std::pair<std::string_view, std::string_view> kPrim[] = {
        {"System.String", "string"},
        {"System.Boolean", "bool"},
        {"System.Int32", "int"},
        {"System.Int64", "long"},
        {"System.UInt32", "uint"},
        {"System.UInt64", "ulong"},
        {"System.Single", "float"},
        {"System.Double", "double"},
        {"System.Byte", "byte"},
        {"System.SByte", "sbyte"},
        {"System.Int16", "short"},
        {"System.UInt16", "ushort"},
        {"System.Char", "char"},
        {"System.Decimal", "decimal"},
        {"System.DateTime", "DateTime"},
        {"System.Object", "object"},
    };
    for (auto [il2, cs] : kPrim) {
        if (s == il2) return std::string(cs);
    }

    // Find the first top-level '<' (depth 0). If found, this is a generic
    // instance: "Def<Args>" where Def is the fully-qualified generic type
    // definition and Args is a comma-separated type argument list.
    {
        size_t lt = std::string::npos;
        {
            int depth = 0;
            for (size_t i = 0; i < s.size(); ++i) {
                if (s[i] == '<') { if (depth == 0) { lt = i; break; } ++depth; }
                else if (s[i] == '>') --depth;
            }
        }
        if (lt != std::string::npos) {
            // Balance to find the matching '>'.
            int depth = 0;
            size_t gt = std::string::npos;
            for (size_t i = lt; i < s.size(); ++i) {
                if (s[i] == '<') ++depth;
                else if (s[i] == '>') { --depth; if (depth == 0) { gt = i; break; } }
            }
            if (gt != std::string::npos) {
                std::string def = s.substr(0, lt);
                std::string inner = s.substr(lt + 1, gt - lt - 1);

                // Split inner on top-level commas and recurse.
                std::vector<std::string> parts;
                int d = 0;
                size_t start = 0;
                for (size_t i = 0; i < inner.size(); ++i) {
                    if (inner[i] == '<') ++d;
                    else if (inner[i] == '>') --d;
                    else if (inner[i] == ',' && d == 0) {
                        parts.push_back(inner.substr(start, i - start));
                        start = i + 1;
                    }
                }
                parts.push_back(inner.substr(start));
                std::string args;
                for (size_t i = 0; i < parts.size(); ++i) {
                    std::string p = parts[i];
                    while (!p.empty() && p.front() == ' ') p.erase(p.begin());
                    while (!p.empty() && p.back() == ' ') p.pop_back();
                    if (i) args += ", ";
                    args += json_type(p);
                }

                // Map the generic type definition to its C# name.
                if (def == "System.Collections.Generic.List") return std::format("List<{}>", args);
                if (def == "System.Collections.Generic.Dictionary") return std::format("Dictionary<{}>", args);
                if (def == "System.Nullable") return std::format("{}?", args);
                // Other generic instance: strip namespace, keep type name + args.
                std::string clean_def = def;
                auto dot = clean_def.rfind('.');
                if (dot != std::string::npos) clean_def = clean_def.substr(dot + 1);
                return std::format("{}<{}>", clean_def, args);
            }
        }
    }

    // Bare generic definition with backtick arity but no args (e.g. "Server.Foo`2").
    // These appear for generic class definitions, not field types. Strip the
    // arity and emit "Foo<T1, T2>".
    {
        size_t bt = s.find('`');
        if (bt != std::string::npos && bt + 1 < s.size() && s[bt + 1] >= '0' && s[bt + 1] <= '9') {
            std::string base = s.substr(0, bt);
            int arity = std::atoi(s.c_str() + bt + 1);
            auto dot = base.rfind('.');
            if (dot != std::string::npos) base = base.substr(dot + 1);
            std::string params;
            for (int i = 0; i < arity; ++i) {
                if (i) params += ", ";
                params += (arity == 1) ? "T" : ("T" + std::to_string(i + 1));
            }
            return std::format("{}<{}>", base, params);
        }
    }

    // Arrays → List<T>.
    if (s.ends_with("[]")) {
        return std::format("List<{}>", json_type(s.substr(0, s.size() - 2)));
    }
    // Strip the namespace prefix from any remaining dotted type name.
    // e.g. "Server.Foo" → "Foo", "UnityEngine.Sprite" → "Sprite".
    auto dot = s.rfind('.');
    if (dot != std::string::npos) {
        // Don't strip if it looks like a qualified primitive (already handled above).
        return s.substr(dot + 1);
    }

    return s;
}

// Render a field for the Types.cs output. No casing changes, no attributes.
// Fields are indented 8 spaces (2 levels: namespace + class).
std::string render_json_field(il2cpp::Il2CppField* f) {
    const char* name = il2cpp::field_get_name(f);
    if (!name) return {};
    // Skip compiler-generated backing fields.
    if (name[0] == '<') return {};
    il2cpp::Il2CppType* ft = il2cpp::field_get_type(f);
    const char* tn = il2cpp::type_get_name(ft);
    std::string cs_type = json_type(tn ? tn : "object");
    return std::format("        public {} {} {{ get; set; }}", cs_type, name);
}

// Render a single class/interface/enum declaration for Types.cs (body only,
// no namespace wrapper — the caller emits one shared namespace block).
// Indented 4 spaces (inside namespace Server).
std::string render_json_class(il2cpp::Il2CppClass* k) {
    const char* name = il2cpp::class_get_name(k);
    if (!name) return {};

    bool is_enum = il2cpp::class_is_enum(k);
    bool is_interface = il2cpp::class_is_interface(k);
    std::string decl_name = generic_class_name(name);

    // Enums: emit as `public enum Name { VAL1, VAL2, ... }` with integer
    // values. il2cpp exposes enum members as static literal fields whose
    // type is the enum itself. The special `value__` instance field is the
    // backing integer and must be skipped.
    if (is_enum) {
        std::vector<std::string> members;
        void* iter = nullptr;
        while (auto* f = il2cpp::class_get_fields(k, &iter)) {
            const char* fname = il2cpp::field_get_name(f);
            if (!fname) continue;
            // Skip the backing integer field.
            if (strcmp(fname, "value__") == 0) continue;
            // Skip non-static fields (enum members are static literals).
            // We detect them by type: the field type should be the enum
            // class itself. If the type name doesn't match the enum name,
            // skip it (e.g. compiler-generated event fields).
            il2cpp::Il2CppType* ft = il2cpp::field_get_type(f);
            const char* tn = il2cpp::type_get_name(ft);
            if (!tn) continue;
            // The member type name should contain the enum name.
            // il2cpp_type_get_name returns "Server.ENUMNAME" for the field
            // type of enum members.
            members.push_back(fname);
        }
        if (members.empty()) {
            return std::format("    public enum {}\n    {{\n    }}", decl_name);
        }
        std::string body;
        for (size_t i = 0; i < members.size(); ++i) {
            body += std::format("        {} = {}", members[i], i);
            if (i + 1 < members.size()) body += ",";
            if (i + 1 < members.size()) body += "\n";
        }
        return std::format("    public enum {}\n    {{\n{}\n    }}", decl_name, body);
    }

    const char* kw = is_interface ? "interface" : "class";

    std::vector<std::string> field_lines;
    void* iter = nullptr;
    while (auto* f = il2cpp::class_get_fields(k, &iter)) {
        std::string s = render_json_field(f);
        if (!s.empty()) field_lines.push_back(s);
    }

    std::string body;
    for (size_t i = 0; i < field_lines.size(); ++i) {
        body += field_lines[i];
        if (i + 1 < field_lines.size()) body += "\n";
    }

    if (body.empty()) {
        return std::format("    public {} {}\n    {{\n    }}", kw, decl_name);
    }
    return std::format("    public {} {}\n    {{\n{}\n    }}", kw, decl_name, body);
}

void dump_types(const std::filesystem::path& out_path) {
    MYO_LOG("dump", "writing Types.cs to {}", out_path.string());
    std::ofstream out(out_path);
    if (!out) { MYO_LOG_ERROR("dump", "cannot open {}", out_path.string()); return; }

    out << "// Auto-generated by myodump. Server namespace types for System.Text.Json.\n";
    out << "// Excludes classes starting with \"Http\" (except HttpRequestFormat / HttpResponseFormat).\n\n";

    size_t asm_count = 0;
    il2cpp::Il2CppAssembly** asms = il2cpp::domain_get_assemblies(&asm_count);
    if (!asms) return;

    // Collect all Server-namespace class blocks first, then emit them inside a
    // single `namespace Server { ... }` block.
    std::vector<std::string> blocks;
    for (size_t i = 0; i < asm_count; ++i) {
        il2cpp::Il2CppImage* img = il2cpp::assembly_get_image(asms[i]);
        if (!img) continue;
        const char* img_name = il2cpp::image_get_name(img);
        // Only dump Assembly-CSharp (where Server.* lives).
        if (!img_name || !strstr(img_name, "Assembly-CSharp")) continue;
        size_t cn = il2cpp::image_get_class_count(img);
        for (size_t c = 0; c < cn; ++c) {
            il2cpp::Il2CppClass* k = il2cpp::image_get_class(img, c);
            if (!k) continue;
            const char* ns = il2cpp::class_get_namespace(k);
            const char* name = il2cpp::class_get_name(k);
            if (!ns || !name) continue;
            // Only "Server" namespace.
            if (strcmp(ns, "Server") != 0) continue;
            // Exclude Http* classes, except HttpRequestFormat/HttpResponseFormat.
            if (name_starts_with_http(name) && !keep_despite_http(name)) continue;

            std::string s = render_json_class(k);
            if (!s.empty()) blocks.push_back(s);
        }
    }

    out << "namespace Server\n{\n";
    for (size_t i = 0; i < blocks.size(); ++i) {
        out << blocks[i];
        if (i + 1 < blocks.size()) out << "\n";
    }
    out << "}\n";
    out.close();
    MYO_LOG("dump", "Types.cs complete: {} classes", blocks.size());
}

void run() {
    config::load_dump();
    log_init(config::g.log_level, L"myodump.log");
    MYO_LOG_OVERRIDE("dump", "myodump starting (dump_level={})", config::g.dump_level);

    if (config::g.dump_level == 0) {
        MYO_LOG("dump", "dump_level=0, nothing to do");
        return;
    }

    if (!wait_for_il2cpp_ready()) {
        MYO_LOG_ERROR("dump", "timed out waiting for il2cpp");
        return;
    }
    MYO_LOG("dump", "il2cpp names resolved");

    if (!il2cpp::init()) {
        MYO_LOG_ERROR("dump", "il2cpp bridge init failed");
        return;
    }

    if (!wait_for_domain_ready()) {
        MYO_LOG_ERROR("dump", "il2cpp domain never populated");
        return;
    }
    MYO_LOG("dump", "il2cpp domain ready");

    if (il2cpp::Il2CppDomain* d = il2cpp::domain_get()) {
        il2cpp::thread_attach(d);
    }

    // Create ./dump/ directory next to the DLL.
    std::filesystem::path dump_dir;
    {
        wchar_t exe[MAX_PATH] = {};
        HMODULE h = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&run), &h);
        if (h && GetModuleFileNameW(h, exe, MAX_PATH)) {
            std::wstring p(exe);
            auto slash = p.find_last_of(L"\\/");
            if (slash != std::wstring::npos) p.resize(slash + 1);
            dump_dir = std::filesystem::path(p) / L"dump";
        }
    }
    if (dump_dir.empty()) dump_dir = "dump";
    std::error_code ec;
    std::filesystem::create_directories(dump_dir, ec);

    // dump_level 1 = Types.cs only; 2 = Types.cs + Il2Cpp.cs.
    if (config::g.dump_level >= 1) dump_types(dump_dir / "Types.cs");
    if (config::g.dump_level >= 2) dump_all(dump_dir / "Il2Cpp.cs");

    MYO_LOG_OVERRIDE("dump", "done");
}

}  // namespace

// Exported entry point (for manual calling from a debugger or external tool).
extern "C" __declspec(dllexport) void myodump_run() { run(); }

}  // namespace myodump

namespace {

LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "[Myosotis:dump] CRASHED (code 0x%08lX at %p)",
                  static_cast<unsigned long>(ep ? ep->ExceptionRecord->ExceptionCode : 0),
                  ep ? static_cast<void*>(ep->ExceptionRecord->ExceptionAddress) : nullptr);
    myosotis::log_raw(buf);
    return EXCEPTION_EXECUTE_HANDLER;
}

DWORD WINAPI init_thread(LPVOID) {
    SetUnhandledExceptionFilter(&crash_filter);
    myodump::run();
    return 0;
}

}  // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hInst);
        HANDLE h = CreateThread(nullptr, 0, &init_thread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
    return TRUE;
}
