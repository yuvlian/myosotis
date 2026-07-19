# This script first scans UnityPlayer.dll to generate a mapping of obfuscated function names to their unobfuscated names.
# Then it creates a binja script to rename the obfuscated function names in Binary Ninja.
# Then it patches both UnityPlayer.dll and GameAssembly.dll to use canonical names instead, so we don't need to update
# namings in LethInEx or other tools.
import json
import os.path

import iced_x86.Code
from iced_x86 import *
import pefile
import argparse
import sys

il2cpp_names = [
    "il2cpp_init",
    "il2cpp_init_utf16",
    "il2cpp_shutdown",
    "il2cpp_set_config_dir",
    "il2cpp_set_data_dir",
    "il2cpp_set_temp_dir",
    "il2cpp_set_commandline_arguments",
    "il2cpp_set_commandline_arguments_utf16",
    "il2cpp_set_config_utf16",
    "il2cpp_set_config",
    "il2cpp_set_memory_callbacks",
    "il2cpp_memory_pool_set_region_size",
    "il2cpp_memory_pool_get_region_size",
    "il2cpp_get_corlib",
    "il2cpp_add_internal_call",
    "il2cpp_resolve_icall",
    "il2cpp_alloc",
    "il2cpp_free",
    "il2cpp_array_class_get",
    "il2cpp_array_length",
    "il2cpp_array_get_byte_length",
    "il2cpp_array_new",
    "il2cpp_array_new_specific",
    "il2cpp_array_new_full",
    "il2cpp_bounded_array_class_get",
    "il2cpp_array_element_size",
    "il2cpp_assembly_get_image",
    "il2cpp_class_for_each",
    "il2cpp_class_enum_basetype",
    "il2cpp_class_is_inited",
    "il2cpp_class_is_generic",
    "il2cpp_class_is_inflated",
    "il2cpp_class_is_assignable_from",
    "il2cpp_class_is_subclass_of",
    "il2cpp_class_has_parent",
    "il2cpp_class_from_il2cpp_type",
    "il2cpp_class_from_name",
    "il2cpp_class_from_system_type",
    "il2cpp_class_get_element_class",
    "il2cpp_class_get_events",
    "il2cpp_class_get_fields",
    "il2cpp_class_get_nested_types",
    "il2cpp_class_get_interfaces",
    "il2cpp_class_get_properties",
    "il2cpp_class_get_property_from_name",
    "il2cpp_class_get_field_from_name",
    "il2cpp_class_get_methods",
    "il2cpp_class_get_method_from_name",
    "il2cpp_class_get_name",
    "il2cpp_type_get_name_chunked",
    "il2cpp_class_get_namespace",
    "il2cpp_class_get_parent",
    "il2cpp_class_get_declaring_type",
    "il2cpp_class_instance_size",
    "il2cpp_class_num_fields",
    "il2cpp_class_is_valuetype",
    "il2cpp_class_value_size",
    "il2cpp_class_is_blittable",
    "il2cpp_class_get_flags",
    "il2cpp_class_is_abstract",
    "il2cpp_class_is_interface",
    "il2cpp_class_array_element_size",
    "il2cpp_class_from_type",
    "il2cpp_class_get_type",
    "il2cpp_class_get_type_token",
    "il2cpp_class_has_attribute",
    "il2cpp_class_has_references",
    "il2cpp_class_is_enum",
    "il2cpp_class_get_image",
    "il2cpp_class_get_assemblyname",
    "il2cpp_class_get_rank",
    "il2cpp_class_get_data_size",
    "il2cpp_class_get_static_field_data",
    "il2cpp_stats_dump_to_file",
    "il2cpp_stats_get_value",
    "il2cpp_domain_get",
    "il2cpp_domain_assembly_open",
    "il2cpp_domain_get_assemblies",
    "il2cpp_raise_exception",
    "il2cpp_exception_from_name_msg",
    "il2cpp_get_exception_argument_null",
    "il2cpp_format_exception",
    "il2cpp_format_stack_trace",
    "il2cpp_unhandled_exception",
    "il2cpp_native_stack_trace",
    "il2cpp_field_get_flags",
    "il2cpp_field_get_from_reflection",
    "il2cpp_field_get_name",
    "il2cpp_field_get_parent",
    "il2cpp_field_get_object",
    "il2cpp_field_get_offset",
    "il2cpp_field_get_type",
    "il2cpp_field_get_value",
    "il2cpp_field_get_value_object",
    "il2cpp_field_has_attribute",
    "il2cpp_field_set_value",
    "il2cpp_field_static_get_value",
    "il2cpp_field_static_set_value",
    "il2cpp_field_set_value_object",
    "il2cpp_field_is_literal",
    "il2cpp_gc_collect",
    "il2cpp_gc_collect_a_little",
    "il2cpp_gc_start_incremental_collection",
    "il2cpp_gc_disable",
    "il2cpp_gc_enable",
    "il2cpp_gc_is_disabled",
    "il2cpp_gc_set_mode",
    "il2cpp_gc_get_max_time_slice_ns",
    "il2cpp_gc_set_max_time_slice_ns",
    "il2cpp_gc_is_incremental",
    "il2cpp_gc_get_used_size",
    "il2cpp_gc_get_heap_size",
    "il2cpp_gc_wbarrier_set_field",
    "il2cpp_gc_has_strict_wbarriers",
    "il2cpp_gc_set_external_allocation_tracker",
    "il2cpp_gc_set_external_wbarrier_tracker",
    "il2cpp_gc_foreach_heap",
    "il2cpp_stop_gc_world",
    "il2cpp_start_gc_world",
    "il2cpp_gc_alloc_fixed",
    "il2cpp_gc_free_fixed",
    "il2cpp_gchandle_new",
    "il2cpp_gchandle_new_weakref",
    "il2cpp_gchandle_get_target",
    "il2cpp_gchandle_free",
    "il2cpp_gchandle_foreach_get_target",
    "il2cpp_object_header_size",
    "il2cpp_array_object_header_size",
    "il2cpp_offset_of_array_length_in_array_object_header",
    "il2cpp_offset_of_array_bounds_in_array_object_header",
    "il2cpp_allocation_granularity",
    "il2cpp_unity_liveness_allocate_struct",
    "il2cpp_unity_liveness_calculation_from_root",
    "il2cpp_unity_liveness_calculation_from_statics",
    "il2cpp_unity_liveness_finalize",
    "il2cpp_unity_liveness_free_struct",
    "il2cpp_method_get_return_type",
    "il2cpp_method_get_declaring_type",
    "il2cpp_method_get_name",
    "il2cpp_method_get_from_reflection",
    "il2cpp_method_get_object",
    "il2cpp_method_is_generic",
    "il2cpp_method_is_inflated",
    "il2cpp_method_is_instance",
    "il2cpp_method_get_param_count",
    "il2cpp_method_get_param",
    "il2cpp_method_get_class",
    "il2cpp_method_has_attribute",
    "il2cpp_method_get_flags",
    "il2cpp_method_get_token",
    "il2cpp_method_get_param_name",
    "il2cpp_property_get_flags",
    "il2cpp_property_get_get_method",
    "il2cpp_property_get_set_method",
    "il2cpp_property_get_name",
    "il2cpp_property_get_parent",
    "il2cpp_object_get_class",
    "il2cpp_object_get_size",
    "il2cpp_object_get_virtual_method",
    "il2cpp_object_new",
    "il2cpp_object_unbox",
    "il2cpp_value_box",
    "il2cpp_monitor_enter",
    "il2cpp_monitor_try_enter",
    "il2cpp_monitor_exit",
    "il2cpp_monitor_pulse",
    "il2cpp_monitor_pulse_all",
    "il2cpp_monitor_wait",
    "il2cpp_monitor_try_wait",
    "il2cpp_runtime_invoke",
    "il2cpp_runtime_invoke_convert_args",
    "il2cpp_runtime_class_init",
    "il2cpp_runtime_object_init",
    "il2cpp_runtime_object_init_exception",
    "il2cpp_runtime_unhandled_exception_policy_set",
    "il2cpp_string_length",
    "il2cpp_string_chars",
    "il2cpp_string_new",
    "il2cpp_string_new_len",
    "il2cpp_string_new_utf16",
    "il2cpp_string_new_wrapper",
    "il2cpp_string_intern",
    "il2cpp_string_is_interned",
    "il2cpp_thread_current",
    "il2cpp_thread_attach",
    "il2cpp_thread_detach",
    "il2cpp_is_vm_thread",
    "il2cpp_current_thread_walk_frame_stack",
    "il2cpp_thread_walk_frame_stack",
    "il2cpp_current_thread_get_top_frame",
    "il2cpp_thread_get_top_frame",
    "il2cpp_current_thread_get_frame_at",
    "il2cpp_thread_get_frame_at",
    "il2cpp_current_thread_get_stack_depth",
    "il2cpp_thread_get_stack_depth",
    "il2cpp_override_stack_backtrace",
    "il2cpp_type_get_object",
    "il2cpp_type_get_type",
    "il2cpp_type_get_class_or_element_class",
    "il2cpp_type_get_name",
    "il2cpp_type_is_byref",
    "il2cpp_type_get_attrs",
    "il2cpp_type_equals",
    "il2cpp_type_get_assembly_qualified_name",
    "il2cpp_type_get_reflection_name",
    "il2cpp_type_is_static",
    "il2cpp_type_is_pointer_type",
    "il2cpp_image_get_assembly",
    "il2cpp_image_get_name",
    "il2cpp_image_get_filename",
    "il2cpp_image_get_entry_point",
    "il2cpp_image_get_class_count",
    "il2cpp_image_get_class",
    "il2cpp_capture_memory_snapshot",
    "il2cpp_free_captured_memory_snapshot",
    "il2cpp_set_find_plugin_callback",
    "il2cpp_register_log_callback",
    "il2cpp_debugger_set_agent_options",
    "il2cpp_is_debugger_attached",
    "il2cpp_register_debugger_agent_transport",
    "il2cpp_debug_foreach_method",
    "il2cpp_debug_get_method_info",
    "il2cpp_unity_install_unitytls_interface",
    "il2cpp_custom_attrs_from_class",
    "il2cpp_custom_attrs_from_method",
    "il2cpp_custom_attrs_from_field",
    "il2cpp_custom_attrs_get_attr",
    "il2cpp_custom_attrs_has_attr",
    "il2cpp_custom_attrs_construct",
    "il2cpp_custom_attrs_free",
    "il2cpp_class_set_userdata",
    "il2cpp_class_get_userdata_offset",
    "il2cpp_set_default_thread_affinity",
    "il2cpp_unity_set_android_network_up_state_func",
]


def function_body(pe: pefile.PE, function_address: int):
    body = pe.get_data(function_address, 0x10000)
    body = body[:body.index(b"\xcc\xcc\xcc")]
    return Decoder(64, body, ip=function_address)


def callees(pe: pefile.PE, function_address: int):
    body = function_body(pe, function_address)
    for inst in body:
        if inst.code == iced_x86.Code.CALL_REL32_64:
            yield inst.memory_displacement


def list_lea_strings(pe: pefile.PE, function_address: int):
    for inst in function_body(pe, function_address):
        if inst.mnemonic != iced_x86.Mnemonic.LEA or inst.memory_base != iced_x86.Register.RIP:
            continue
        yield pe.get_string_at_rva(inst.memory_displacement, max_length=16)


def analyze_mapping(unity_player_pe: pefile.PE) -> dict[str, str]:
    unity_player_pe.parse_data_directories(
        directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
    # Scan for UnityMain entrypoint
    unity_main = next(
        e for e in unity_player_pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name == b"UnityMain")
    print(f"UnityMain entrypoint found at 0x{unity_main.address:08x}")
    # Scan for body
    unity_body_addr = next(x for x in callees(
        unity_player_pe, unity_main.address))
    print(f"UnityMain body found at 0x{unity_body_addr:08x}")
    # Scan for callees
    unity_main_impl = next(outer for outer in callees(
        unity_player_pe, unity_body_addr) if b"il2cpp_data" in list_lea_strings(unity_player_pe, outer))
    print(f"UnityMain implementation found at 0x{unity_main_impl:08x}")
    # Scan for names
    load_il2cpp = list(callees(unity_player_pe, unity_main_impl))[1]
    print(f"LoadIl2CPP found at 0x{load_il2cpp:08x}")
    found_names = list(map(lambda x: x.decode(), filter(lambda x: len(
        x) == 11, list_lea_strings(unity_player_pe, load_il2cpp))))
    assert len(found_names) == len(
        il2cpp_names), f"Expected {len(il2cpp_names)} names, found {len(found_names)}"
    return dict(zip(il2cpp_names, found_names))


def add_file_suffix(path: str, suffix: str):
    directory, filename = os.path.split(path)
    name, extension = os.path.splitext(filename)
    if extension:
        new_filename = f"{name}.{suffix}{extension}"
    else:
        new_filename = f"{name}.{suffix}"
    return os.path.join(directory, new_filename)


def zero_file(path: str, game_assembly_pe: pefile.PE):
    il2cpp_section = next(
        section for section in game_assembly_pe.sections if b"il2cpp" in section.Name)
    offset = game_assembly_pe.get_offset_from_rva(
        il2cpp_section.VirtualAddress)
    new_path = add_file_suffix(path, "zeroed")
    with open(path, "rb") as dll_input:
        prefix = dll_input.read(offset)
        dll_input.seek(offset + il2cpp_section.Misc_VirtualSize, os.SEEK_SET)
        suffix = dll_input.read()
    with open(new_path, "wb") as dll_output:
        dll_output.write(prefix)
        dll_output.write(b"\xcc" * il2cpp_section.Misc_VirtualSize)
        dll_output.write(suffix)
    print(
        f"Zeroed out {offset:08x}-{il2cpp_section.Misc_VirtualSize:08x} bytes in {new_path}.")


def write_binja_script(output_path: str, mapping: dict):
    print(f"Writing Binary Ninja script to {output_path}...")
    with open(output_path, "w") as f:
        for unobf_name, obf_name in mapping.items():
            f.write(
                f"bv.define_user_symbol(Symbol(SymbolType.FunctionSymbol, bv.symbols['{obf_name}'][0].address, '{unobf_name}'));\n")


def main():
    parser = argparse.ArgumentParser(description="Process a game path.")
    parser.add_argument("--game-path", required=True,
                        help="Path to the game directory.")
    args = parser.parse_args()
    game_path = args.game_path
    unity_player_dll = os.path.join(game_path, "UnityPlayer.dll")
    game_assembly_dll = os.path.join(game_path, "GameAssembly.dll")
    if not os.path.isfile(unity_player_dll):
        print(f"Error: UnityPlayer.dll not found at {unity_player_dll}")
        sys.exit(1)
    if not os.path.isfile(game_assembly_dll):
        print(f"Error: GameAssembly.dll not found at {game_assembly_dll}")
        sys.exit(1)

    print("Loading GameAssembly.dll and UnityPlayer.dll...")
    unity_player_pe = pefile.PE(unity_player_dll, fast_load=True)
    if not unity_player_pe.is_dll():
        print(f"Error: {unity_player_dll} is not a valid DLL file.")
        sys.exit(1)
    game_assembly_pe = pefile.PE(game_assembly_dll, fast_load=True)
    if not game_assembly_pe.is_dll():
        print(f"Error: {game_assembly_dll} is not a valid DLL file.")
        sys.exit(1)

    mapping = analyze_mapping(unity_player_pe)
    print(f"Mapping analysis complete. Writing to mapping.json")
    with open("mapping.json", "w") as f:
        json.dump(mapping, f, indent=2)

    zero_file(game_assembly_dll, game_assembly_pe)
    print("Patching complete.")

    write_binja_script("binja_rename_exports.py.txt", mapping)
    print("Done.")


if __name__ == "__main__":
    main()
