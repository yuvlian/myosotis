// Byte-pattern disassembler for il2cpp name resolution.
//
// We don't need a full x86-64 decoder. The only facts we need from
// UnityPlayer.dll's code are:
//   (1) CALL rel32 — to walk UnityMain -> body -> impl -> LoadIl2CPP.
//   (2) LEA reg, [rip + disp32] — to read the 11-char obfuscated name strings.
//
// x86-64 encodings relevant here:
//   E8 disp32                 — CALL rel32 (5 bytes)
//   48 8D ?? 05 disp32        — LEA r64, [rip+disp32] (7 bytes, REX.W=1, REX.R encodes the dst reg in the ModRM byte)
//   4C 8D ?? 05 disp32        — same with REX.R=1 (r8-r15 as dst)
//
// Where ?? is the ModRM byte: mod=00, rm=101 (rip-relative) means ModRM == 0x05,
// 0x0D, 0x15, ... 0x3D (low 3 bits = 5, top 2 bits = 0). The dst reg is encoded
// in ModRM bits 3-5 plus REX.R; we don't actually care which register.
//
// We validate every hit:
//   - CALL target must fall inside a mapped .text-ish section.
//   - LEA target must be inside the image and point to a NUL-terminated 11-char
//     ASCII alphanumeric string (the obfuscated-name predicate from a.rs).
//
// This is enough to reproduce analyze_mapping.py's result. The "body length"
// terminator is a run of 0xCC (int3) bytes, same heuristic as a.rs.

#pragma once
#include "pe.hpp"
#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

namespace myosotis::scan {

// Call target (RVA) if the byte at `ip` is `E8 disp32` and the target lands
// inside the image; otherwise 0.
size_t try_call_rel32(const myosotis::pe::Image& img, size_t ip);

// LEA rip-relative target (RVA) if the instruction at `ip` matches
// `48/4C 8D ?? 05 disp32`; otherwise 0.
size_t try_lea_rip(const myosotis::pe::Image& img, size_t ip);

// Predicate from a.rs: exactly 11 chars, ASCII alphanumeric or '_'.
bool is_obf_name(const std::string& s);

// Collect all 11-char rip-relative LEA strings inside the function body that
// starts at `entry_rva`, in instruction order. The body extends from
// `entry_rva` up to the first 0xCC 0xCC 0xCC run (or max_bytes).
std::vector<std::string> lea_strings_in_body(const myosotis::pe::Image& img,
                                             size_t entry_rva,
                                             size_t max_bytes = 0x10000);

// Collect all CALL rel32 targets inside a body, in instruction order.
std::vector<size_t> call_targets_in_body(const myosotis::pe::Image& img,
                                          size_t entry_rva,
                                          size_t max_bytes = 0x10000);

}  // namespace myosotis::scan
