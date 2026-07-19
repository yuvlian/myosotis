// Method-pointer hook implementation.
#include "hook.hpp"
#include "log.hpp"
#include <windows.h>
#include <cstring>

namespace myosotis::hook {

namespace {
// MethodInfo layout we depend on: methodPointer at offset 0.
// (Matches the real Il2CppMethodInfo for Unity 2021+; offset 0 is `methodPointer`,
//  an Il2CppMethodPointer / void*.)
constexpr size_t kMethodPointerOffset = 0;

void* write_method_ptr(::myosotis::il2cpp::Il2CppMethod* method, void* new_ptr) {
    if (!method) return nullptr;
    void* slot = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(method) + kMethodPointerOffset);
    void* old;
    // The il2cpp metadata pages are typically read-only; VirtualProtect to RW for the write.
    DWORD old_prot = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_prot)) {
        MYO_LOG("hook", "VirtualProtect(RW) failed: {}", GetLastError());
        return nullptr;
    }
    std::memcpy(&old, slot, sizeof(void*));
    std::memcpy(slot, &new_ptr, sizeof(void*));
    DWORD dummy = 0;
    VirtualProtect(slot, sizeof(void*), old_prot, &dummy);
    return old;
}
}  // namespace

void* install(::myosotis::il2cpp::Il2CppMethod* method, void* new_ptr) {
    if (!method || !new_ptr) return nullptr;
    void* old = write_method_ptr(method, new_ptr);
    if (!old) return nullptr;
    return old;
}

void uninstall(::myosotis::il2cpp::Il2CppMethod* method, void* original) {
    if (!method || !original) return;
    write_method_ptr(method, original);
}

}  // namespace myosotis::hook
