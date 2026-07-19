// Byte-pattern scan implementation.
//
// Strategy: walk the body byte-by-byte. At each position, test for the two
// patterns (E8 disp32 CALL, 48/4C 8D ?? 05 disp32 LEA-rip). Each hit is
// validated against the image:
//   - CALL target RVA must be inside the image and non-zero.
//   - LEA target must point to a NUL-terminated 11-char obfuscated name.
//
// False positives are possible (immediate bytes inside other instructions can
// look like E8 or 8D), but the validation is tight enough — especially the
// 11-char-string predicate — that in practice the only surviving LEA hits are
// the real obfuscated-name loads. CALL false positives are filtered downstream
// by the LoadIl2CPP selection logic (we only take callees that themselves
// contain the "il2cpp_data" string, exactly like a.rs).
//
// We do NOT advance by instruction length: we advance byte-by-byte and let
// validation reject spurious matches. This is O(n) and matches what a naive
// regex scan would produce; the 11-char gate is the real filter.

#include "scan.hpp"
#include "log.hpp"
#include <cstring>
#include <bit>
#include <algorithm>
#include <ranges>
#include <array>
#include <string_view>

namespace myosotis::scan {

namespace {

// Read a little-endian int32 at `ip` within the image. Returns 0 if OOB.
// C++23 std::bit_cast replaces the memcpy dance — it's a compile-time-constant
// reinterpretation that the optimizer treats as a plain load.
int32_t rd_i32(const myosotis::pe::Image& img, size_t ip) {
    if (ip + 4 > img.size()) return 0;
    std::array<uint8_t, 4> raw{};
    std::memcpy(raw.data(), img.rva(ip), 4);
    return std::bit_cast<int32_t>(raw);
}

// Predicate from a.rs: exactly 11 chars, ASCII alphanumeric or '_'.
// C++20 ranges: all_of with a character-class lambda is clearer than the
// manual for-loop it replaced.
constexpr bool is_obf_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') || c == '_';
}

}  // namespace

size_t try_call_rel32(const myosotis::pe::Image& img, size_t ip) {
    if (ip + 5 > img.size()) return 0;
    if (img.rva(ip)[0] != 0xE8) return 0;
    int32_t disp = rd_i32(img, ip + 1);
    // CALL rel32: target = next_ip + disp
    size_t target = static_cast<size_t>(static_cast<int64_t>(ip) + 5 + disp);
    if (target == 0 || target >= img.size()) return 0;
    return target;
}

size_t try_lea_rip(const myosotis::pe::Image& img, size_t ip) {
    // 48 8D MM 05 disp32  or  4C 8D MM 05 disp32
    // where MM is any byte with mod=00, rm=101 => (MM & 0xC7) == 0x05
    if (ip + 7 > img.size()) return 0;
    const uint8_t* p = img.rva(ip);
    if (p[0] != 0x48 && p[0] != 0x4C) return 0;
    if (p[1] != 0x8D) return 0;
    if ((p[2] & 0xC7) != 0x05) return 0;  // mod=00, rm=101 (rip-relative)
    int32_t disp = rd_i32(img, ip + 3);
    // LEA target = next_ip + disp
    size_t target = static_cast<size_t>(static_cast<int64_t>(ip) + 7 + disp);
    if (target >= img.size()) return 0;
    return target;
}

bool is_obf_name(const std::string& s) {
    return s.size() == 11 && std::ranges::all_of(s, is_obf_char);
}

namespace {
// Find the body length: from entry, up to the first 0xCC 0xCC 0xCC run,
// capped at max_bytes. C++23 ranges::search finds the 3-byte sentinel without
// a manual index loop.
size_t body_len(const myosotis::pe::Image& img, size_t entry, size_t max_bytes) {
    size_t cap = std::min(img.size() - entry, max_bytes);
    auto body = std::span<const uint8_t>{img.rva(entry), cap};
    constexpr std::array<uint8_t, 3> sentinel{0xCC, 0xCC, 0xCC};
    auto it = std::ranges::search(body, sentinel);
    if (it.begin() == body.end()) return cap;
    return static_cast<size_t>(it.begin() - body.begin());
}
}  // namespace

std::vector<std::string> lea_strings_in_body(const myosotis::pe::Image& img,
                                             size_t entry_rva,
                                             size_t max_bytes) {
    std::vector<std::string> out;
    size_t len = body_len(img, entry_rva, max_bytes);
    for (size_t ip = entry_rva; ip < entry_rva + len; ++ip) {
        size_t t = try_lea_rip(img, ip);
        if (t == 0) continue;
        std::string s = img.read_ascii(t, 16);
        if (!is_obf_name(s)) continue;
        out.push_back(s);
    }
    return out;
}

std::vector<size_t> call_targets_in_body(const myosotis::pe::Image& img,
                                           size_t entry_rva,
                                           size_t max_bytes) {
    std::vector<size_t> out;
    size_t len = body_len(img, entry_rva, max_bytes);
    for (size_t ip = entry_rva; ip < entry_rva + len; ++ip) {
        size_t t = try_call_rel32(img, ip);
        if (t == 0) continue;
        out.push_back(t);
    }
    return out;
}

}  // namespace myosotis::scan
