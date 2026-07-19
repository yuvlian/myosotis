// Config: reads myosotis.ini next to the DLL using Windows' GetPrivateProfileStringW.
//
// [myosotis]
// token = <hardcoded steam JWT>
// server = https://api.lethelc.site/
// serverinfos_url = https://raw.githubusercontent.com/LEAGUE-OF-NINE/motions-schema/refs/heads/main/serverinfos.json
// log_level = 1

#pragma once
#include <string>
#include <cstdint>

namespace myosotis::config {

struct Config {
    std::wstring token;           // hardcoded steam JWT
    std::wstring server;          // redirect target, e.g. https://api.lethelc.site/
    std::wstring serverinfos_url; // raw github url for serverinfos_*
    int log_level = 1;
};

extern Config g;

// Load from myosotis.ini next to this DLL. Returns false on failure.
bool load();

}  // namespace myosotis::config
