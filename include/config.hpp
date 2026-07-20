// Config: reads myosotis.ini next to the DLL using Windows' GetPrivateProfileStringW.
//
// [myosotis]
// token = hardcoded steam token
// server = http://127.0.0.1:3000/
// serverinfos_url = http://127.0.0.1:3000/serverinfos.json
// log_level = 1  (0 = only error, 1 = error & info, 2 = error & info & debug)
// dump_level = 1 (0 = off, 1 = types, 2 = types + il2cpp)

#pragma once
#include <string>
#include <cstdint>

namespace myosotis::config {

struct Config {
    std::wstring token;           // hardcoded steam JWT
    std::wstring server;          // redirect target, e.g. https://api.lethelc.site/
    std::wstring serverinfos_url; // raw github url for serverinfos_*
    int log_level = 1;
    int dump_level = 0;  // 0=off, 1=types only, 2=types+il2cpp
};

extern Config g;

// Load from myosotis.ini next to this DLL. Returns false on failure.
// Reads all keys (token, server, serverinfos_url, log_level, dump_level).
bool load();

// Load only dump-related keys (log_level, dump_level) from myosotis.ini.
// Used by myodump.dll which doesn't need the patch config. Returns false on failure.
bool load_dump();

}  // namespace myosotis::config
