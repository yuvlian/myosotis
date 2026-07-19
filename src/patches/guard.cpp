#include <windows.h>
// GuardPatch implementation.
#include "patches/guard.hpp"
#include "il2cpp.hpp"
#include "log.hpp"
#include <cstring>

namespace myosotis::patches {

namespace {

// ---- Stubs. Signature is "everything fits in a pointer slot, return void*".
// il2cpp method dispatch passes (instance, args..., MethodInfo*). Our stubs
// ignore all args and just return the typed default.

// void-return stub: return nothing (rax is whatever; callers ignore it).
extern "C" void __cdecl myosotis_stub_void() {}

// bool-return stub: return false (0 in al).
extern "C" bool __cdecl myosotis_stub_bool() { return false; }

// string-return stub: return an empty il2cpp string. We can't easily allocate
// one without a thread-attached il2cpp context here (stubs may run on arbitrary
// il2cpp threads, which is fine, but we need the il2cpp string API). We resolve
// il2cpp_string_new lazily and cache the empty string pointer.
extern "C" void* __cdecl myosotis_stub_string() {
    static void* empty_str = nullptr;
    if (!empty_str) empty_str = myosotis::il2cpp::string_new("");
    return empty_str;
}

void* stub_for_return_type(il2cpp::Il2CppType* ret_type) {
    if (!ret_type) return reinterpret_cast<void*>(&myosotis_stub_void);
    int t = il2cpp::type_get_type(ret_type);
    // Il2CppTypeEnum values: 0x0E == STRING, 0x02 == BOOLEAN.
    // We treat everything we don't recognize as void.
    switch (t) {
        case 0x0E: return reinterpret_cast<void*>(&myosotis_stub_string);
        case 0x02: return reinterpret_cast<void*>(&myosotis_stub_bool);
        default:   return reinterpret_cast<void*>(&myosotis_stub_void);
    }
}

int install_on_class(il2cpp::Il2CppClass* klass) {
    if (!klass) return 0;
    // Make sure the class is initialized so MethodInfo is fully populated.
    il2cpp::runtime_class_init(klass);

    int n = 0;
    void* iter = nullptr;
    while (auto* m = il2cpp::class_get_methods(klass, &iter)) {
        const char* name = il2cpp::method_get_name(m);
        if (!name) continue;
        // Skip special-name members (properties/operators) like the C# version
        // skips IsSpecialName, and skip anything literally named "Invoke" /
        // "Invoke*" to avoid stubbing reflection helpers.
        if (name[0] == 'g' && strncmp(name, "get_", 4) == 0) { /* still stub */ }
        if (name[0] == 's' && strncmp(name, "set_", 4) == 0) { /* still stub */ }
        if (strstr(name, "Invoke")) continue;

        void* new_ptr = stub_for_return_type(il2cpp::method_get_return_type(m));
        // Overwrite MethodInfo->methodPointer (offset 0).
        // We don't bother with VirtualProtect here: the guard-patch overwrites
        // many methods and the metadata pages are generally writable via the
        // same VirtualProtect dance in hook.cpp. Reuse that path via the hook API.
        void** slot = reinterpret_cast<void**>(m);
        DWORD old_prot = 0;
        if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_prot)) {
            *slot = new_ptr;
            DWORD dummy = 0;
            VirtualProtect(slot, sizeof(void*), old_prot, &dummy);
            ++n;
        }
    }
    return n;
}

}  // namespace

bool install_guard() {
    MYO_LOG("guard", "scanning assemblies for JsonExtensions...");
    size_t count = 0;
    il2cpp::Il2CppAssembly** asms = il2cpp::domain_get_assemblies(&count);
    if (!asms) { MYO_LOG("guard", "no assemblies"); return false; }

    int total = 0;
    for (size_t i = 0; i < count; ++i) {
        il2cpp::Il2CppImage* img = il2cpp::assembly_get_image(asms[i]);
        if (!img) continue;
        const char* name = il2cpp::image_get_name(img);
        if (!name || !strstr(name, "JsonExtensions")) continue;
        MYO_LOG("guard", "found image: {}", name);

        size_t cn = il2cpp::image_get_class_count(img);
        for (size_t c = 0; c < cn; ++c) {
            total += install_on_class(il2cpp::image_get_class(img, c));
        }
    }
    MYO_LOG("guard", "stubbed {} methods", total);

    // Patch Environment.Exit / Application.Quit / Environment.FailFast by
    // name on their declaring classes. These live in mscorlib / UnityEngine.Core.
    auto stub_by_name = [](const char* ns, const char* klass, const char* method, void* stub) {
        il2cpp::Il2CppClass* k = il2cpp::find_class(ns, klass);
        if (!k) { MYO_LOG("guard", "class {}.{} not found", ns, klass); return; }
        il2cpp::Il2CppMethod* m = il2cpp::class_get_method_from_name(k, method, 1);
        if (!m) m = il2cpp::class_get_method_from_name(k, method, 0);
        if (!m) { MYO_LOG("guard", "method {}.{} not found", klass, method); return; }
        void** slot = reinterpret_cast<void**>(m);
        DWORD op = 0;
        if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &op)) {
            *slot = stub;
            DWORD d = 0; VirtualProtect(slot, sizeof(void*), op, &d);
            MYO_LOG("guard", "stubbed {}.{}", klass, method);
        }
    };

    stub_by_name("System", "Environment", "Exit", reinterpret_cast<void*>(&myosotis_stub_void));
    stub_by_name("UnityEngine", "Application", "Quit", reinterpret_cast<void*>(&myosotis_stub_void));
    stub_by_name("System", "Environment", "FailFast", reinterpret_cast<void*>(&myosotis_stub_void));

    return true;
}

}  // namespace myosotis::patches
