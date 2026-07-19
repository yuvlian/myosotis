// DllMain: spawn the init thread on attach. We never block the loader.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "init.hpp"

namespace {
DWORD WINAPI init_thread(LPVOID) {
    myosotis::init_all();
    return 0;
}
}  // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(GetModuleHandleW(nullptr));
        HANDLE h = CreateThread(nullptr, 0, &init_thread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
    return TRUE;
}
