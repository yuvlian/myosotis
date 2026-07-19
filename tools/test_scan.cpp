// Standalone test for the il2cpp name scan.
// Loads UnityPlayer.dll from the Limbus game dir, runs the byte-pattern scan,
// prints the count + first 5 entries, and diffs against the embedded map.
// Exits 0 on match, 1 on mismatch.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <print>
#include <string>
#include <unordered_map>
#include "pe.hpp"
#include "scan.hpp"
#include "il2cpp_names.hpp"
#include "il2cpp_names.generated.h"
#include "il2cpp_map.generated.h"

int main() {
    const wchar_t* game_dir =
        L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Limbus Company";
    wchar_t up_path[MAX_PATH], ga_path[MAX_PATH];
    swprintf_s(up_path, MAX_PATH, L"%s\\UnityPlayer.dll", game_dir);
    swprintf_s(ga_path, MAX_PATH, L"%s\\GameAssembly.dll", game_dir);

    HMODULE up = LoadLibraryW(up_path);
    if (!up) { std::println(stderr, "LoadLibraryW(UnityPlayer.dll) failed: {}", GetLastError()); return 1; }
    LoadLibraryW(ga_path);

    if (!myosotis::il2cpp_names::resolve()) {
        std::println(stderr, "resolve() FAILED");
        return 1;
    }
    std::println("resolved {} names", myosotis::il2cpp_names::g_map.size());

    int mismatches = 0;
    for (size_t i = 0; i < IL2CPP_MAP_COUNT; ++i) {
        const std::string& got = myosotis::il2cpp_names::lookup(IL2CPP_MAP[i].canonical);
        if (got != IL2CPP_MAP[i].obfuscated) {
            ++mismatches;
            if (mismatches <= 5) {
                std::println(stderr, "MISMATCH {}: runtime={} embedded={}",
                             IL2CPP_MAP[i].canonical, got, IL2CPP_MAP[i].obfuscated);
            }
        }
    }
    std::println("mismatches: {} / {}", mismatches, IL2CPP_MAP_COUNT);

    int n = 0;
    for (const auto& kv : myosotis::il2cpp_names::g_map) {
        if (n++ >= 5) break;
        std::println("  {} -> {}", kv.first, kv.second);
    }
    return mismatches == 0 ? 0 : 1;
}
