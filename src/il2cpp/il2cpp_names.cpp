// il2cpp name resolver implementation.
#include "il2cpp_names.hpp"
#include "scan.hpp"
#include "log.hpp"
#include "il2cpp_names.generated.h"
#include "il2cpp_map.generated.h"
#include <windows.h>
#include <string>
#include <vector>
#include <unordered_set>

namespace myosotis::il2cpp_names {

std::unordered_map<std::string, std::string> g_map;

namespace {

// Find LoadIl2CPP inside UnityPlayer.dll, mirroring a.rs:
//   UnityMain -> body (first call) -> impl (first callee whose LEA strings
//   contain "il2cpp_data") -> LoadIl2CPP (impl's callees[1]).
// Returns 0 on failure.
size_t find_load_il2cpp(const pe::Image& img) {
    size_t unity_main = img.export_rva("UnityMain");
    if (unity_main == 0) { MYO_LOG("scan", "UnityMain not exported"); return 0; }

    auto body_callees = scan::call_targets_in_body(img, unity_main);
    if (body_callees.empty()) { MYO_LOG("scan", "UnityMain has no callees"); return 0; }
    size_t body = body_callees.front();

    auto impl_candidates = scan::call_targets_in_body(img, body);
    size_t impl = 0;
    for (size_t c : impl_candidates) {
        auto strings = scan::lea_strings_in_body(img, c);
        for (const auto& s : strings) {
            if (s == "il2cpp_data") { impl = c; break; }
        }
        if (impl) break;
    }
    if (impl == 0) { MYO_LOG("scan", "unity_main_impl not found"); return 0; }

    auto impl_callees = scan::call_targets_in_body(img, impl);
    if (impl_callees.size() < 2) { MYO_LOG("scan", "impl has <2 callees"); return 0; }
    return impl_callees[1];  // LoadIl2CPP
}

bool resolve_runtime() {
    // UnityPlayer.dll path from the loaded module (matches a.rs).
    wchar_t path[MAX_PATH] = {};
    HMODULE h = GetModuleHandleW(L"UnityPlayer.dll");
    if (!h) { MYO_LOG("scan", "UnityPlayer.dll not loaded yet"); return false; }
    if (!GetModuleFileNameW(h, path, MAX_PATH)) return false;

    pe::Image img;
    if (auto r = img.load(path); !r) { MYO_LOG("scan", "PE load failed: {}", r.error()); return false; }

    size_t load_il2cpp = find_load_il2cpp(img);
    if (load_il2cpp == 0) { MYO_LOG("scan", "LoadIl2CPP not found"); return false; }

    // Collect 11-char LEA strings from LoadIl2CPP and its callees, in order,
    // dedup by first occurrence — same as a.rs.
    std::vector<std::string> found;
    std::unordered_set<std::string> seen;

    auto add_from = [&](size_t rva) {
        for (const auto& s : scan::lea_strings_in_body(img, rva)) {
            if (seen.insert(s).second) found.push_back(s);
        }
    };

    add_from(load_il2cpp);
    for (size_t c : scan::call_targets_in_body(img, load_il2cpp)) add_from(c);

    if (found.size() != IL2CPP_NAMES_COUNT) {
        MYO_LOG("scan", "count mismatch: expected {} found {}",
                 IL2CPP_NAMES_COUNT, found.size());
        return false;
    }

    g_map.clear();
    for (size_t i = 0; i < IL2CPP_NAMES_COUNT; ++i) {
        g_map[IL2CPP_NAMES[i]] = found[i];
    }
    MYO_LOG("scan", "resolved {} il2cpp names at runtime", g_map.size());
    return true;
}

bool resolve_embedded() {
    g_map.clear();
    for (size_t i = 0; i < IL2CPP_MAP_COUNT; ++i) {
        g_map[IL2CPP_MAP[i].canonical] = IL2CPP_MAP[i].obfuscated;
    }
    MYO_LOG("scan", "loaded embedded il2cpp map ({} entries)", g_map.size());
    return true;
}

}  // namespace

bool resolve() {
    if (resolve_runtime()) return true;
    MYO_LOG("scan", "runtime scan failed, falling back to embedded map");
    return resolve_embedded();
}

const std::string& lookup(const char* canonical) {
    static const std::string empty;
    auto it = g_map.find(canonical);
    return it == g_map.end() ? empty : it->second;
}

}  // namespace myosotis::il2cpp_names
