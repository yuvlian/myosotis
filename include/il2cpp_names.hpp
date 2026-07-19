// il2cpp name resolver: reproduces a.rs::init_export_mapping in C++.
//
// Walks UnityPlayer.dll (from disk): UnityMain -> body -> impl -> LoadIl2CPP,
// collects 11-char rip-relative LEA strings from LoadIl2CPP's body in
// instruction order, dedups preserving first occurrence, asserts count == 234,
// zips with IL2CPP_NAMES. Falls back to the embedded generated table if the
// scan fails or yields a count mismatch.
//
// Result is a canonical->obfuscated map; callers then GetProcAddress(GameAssembly,
// obfuscated) to get the actual function pointer.

#pragma once
#include "pe.hpp"
#include <string>
#include <unordered_map>

namespace myosotis::il2cpp_names {

// canonical il2cpp name -> obfuscated GameAssembly.dll export name.
// Populated by resolve(). Empty on failure (caller should then abort).
extern std::unordered_map<std::string, std::string> g_map;

// Resolve the map at runtime. Returns true on success.
// Falls back to the embedded generated table if the runtime scan fails.
bool resolve();

// Lookup; returns "" if not present.
const std::string& lookup(const char* canonical);

}  // namespace myosotis::il2cpp_names
