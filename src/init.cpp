#include "init.hpp"
#include "config.hpp"
#include "il2cpp_names.hpp"
#include "il2cpp.hpp"
#include "log.hpp"
#include "patches/guard.hpp"
#include "patches/login.hpp"
#include "patches/http.hpp"
#include "patches/request.hpp"
#include <windows.h>

namespace myosotis {

namespace {

// Wait for GameAssembly.dll to be loaded AND il2cpp to be initialized.
// We poll il2cpp_domain_get() until it returns non-null; that's the il2cpp
// "ready" signal we need before resolving any class/method.
bool wait_for_il2cpp_ready() {
    for (int i = 0; i < 600; ++i) {  // up to ~60s
        if (!GetModuleHandleW(L"GameAssembly.dll")) { Sleep(100); continue; }
        if (!il2cpp_names::g_map.empty()) return true;
        // Try resolving now: il2cpp_names::resolve scans UnityPlayer.dll which
        // requires it to be loaded. We can resolve names before il2cpp_init
        // completes (it's just a PE disasm), but we can't resolve *classes*
        // until the domain is populated.
        if (il2cpp_names::resolve()) return true;
        Sleep(100);
    }
    return false;
}

// Wait until the il2cpp domain has at least one assembly (il2cpp_init done).
bool wait_for_domain_ready() {
    for (int i = 0; i < 600; ++i) {
        size_t n = 0;
        il2cpp::Il2CppAssembly** asms = il2cpp::domain_get_assemblies(&n);
        if (asms && n > 0) return true;
        Sleep(100);
    }
    return false;
}

}  // namespace

bool init_all() {
    log_init();
    MYO_LOG("init", "myosotis native plugin starting");

    if (!myosotis::config::load()) {
        MYO_LOG("init", "config load failed");
        return false;
    }

    if (!wait_for_il2cpp_ready()) {
        MYO_LOG("init", "il2cpp name resolution failed");
        return false;
    }
    MYO_LOG("init", "il2cpp names resolved");

    if (!wait_for_domain_ready()) {
        MYO_LOG("init", "il2cpp domain never populated");
        return false;
    }
    MYO_LOG("init", "il2cpp domain ready");

    // Attach this thread to il2cpp so our runtime_invoke calls are valid.
    if (il2cpp::Il2CppDomain* d = il2cpp::domain_get()) {
        il2cpp::thread_attach(d);
    }

    if (!il2cpp::init()) {
        MYO_LOG("init", "il2cpp bridge init failed");
        return false;
    }

    // Install patches. Order matters: GuardPatch first (neutralize anti-cheat
    // before any of our other code runs game methods), then Login (auth),
    // then Http (redirects), then Request (transport replacement).
    myosotis::patches::install_guard();
    myosotis::patches::install_login();
    myosotis::patches::install_http();
    myosotis::patches::install_request();

    MYO_LOG("init", "all patches installed");
    return true;
}

}  // namespace myosotis
