// GuardPatch: neutralize the anti-cheat by stubbing every method in the
// JsonExtensions assembly. Mirrors Myosotis/Patches/GuardPatch.cs.
//
// We walk all loaded assemblies, find the one whose image name contains
// "JsonExtensions", and for every declared method on every type in that image
// overwrite MethodInfo->methodPointer with a return-type-aware stub:
//   - void return        -> void_stub (returns immediately)
//   - bool return        -> bool_stub (returns false)
//   - string return      -> str_stub  (returns an empty il2cpp string)
//   - anything else      -> void_stub (zeroed return; safe for value types)
//
// The C# version also patches Environment.Exit / Application.Quit /
// Environment.FailFast. Those are mscorlib/System methods, not il2cpp-compiled
// game code; their MethodInfo pointers live in the il2cpp domain too and the
// same methodPointer-overwrite trick applies. We patch them by name on their
// declaring class.

#pragma once
namespace myosotis::patches {
bool install_guard();
}
