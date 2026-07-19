// Windows PE file parser. No deps beyond Windows SDK.
// Reads a DLL off disk, parses headers + sections into a virtual image buffer,
// resolves export-by-name, reads NUL-terminated ASCII strings by RVA.
//
// Used for the il2cpp name scan: we need to disassemble the *on-disk*
// UnityPlayer.dll (the in-memory copy has relocations already applied, which
// would still work, but reading the file is simpler and matches a.rs).
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <span>
#include <expected>
#include <windows.h>

namespace myosotis::pe {

// A PE image loaded into a virtual-image buffer (section RVAs map directly
// into the buffer). Owned buffer + parsed metadata.
class Image {
public:
    Image() = default;
    ~Image() = default;
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    Image(Image&&) noexcept = default;
    Image& operator=(Image&&) noexcept = default;

    // Load and map a DLL from disk. Returns an error message on failure
    // (C++23 std::expected — caller gets a real diagnostic, not just a bool).
    [[nodiscard]] std::expected<void, std::string> load(const wchar_t* path);

    // Raw virtual-image bytes (indexed by RVA).
    [[nodiscard]] const uint8_t* data() const { return image_.data(); }
    [[nodiscard]] size_t size() const { return image_.size(); }
    [[nodiscard]] uint8_t* rva(size_t rva) {
        return rva < image_.size() ? image_.data() + rva : nullptr;
    }
    [[nodiscard]] const uint8_t* rva(size_t rva) const {
        return rva < image_.size() ? image_.data() + rva : nullptr;
    }
    // Span view of a body at `rva` for `n` bytes — bounds-checked, safe to
    // pass into std::ranges algorithms.
    [[nodiscard]] std::span<uint8_t> span_at(size_t rva, size_t n) {
        if (rva >= image_.size() || n > image_.size() - rva) return {};
        return {image_.data() + rva, n};
    }
    [[nodiscard]] std::span<const uint8_t> span_at(size_t rva, size_t n) const {
        if (rva >= image_.size() || n > image_.size() - rva) return {};
        return {image_.data() + rva, n};
    }

    // Image base from optional header (for translating VA<->RVA).
    [[nodiscard]] uint64_t image_base() const { return image_base_; }

    // Resolve an export by name. Returns RVA (not VA) or 0 if missing.
    [[nodiscard]] size_t export_rva(const char* name) const;

    // Read a NUL-terminated ASCII string at the given RVA.
    // Returns up to max_len bytes; empty on failure.
    [[nodiscard]] std::string read_ascii(size_t rva, size_t max_len = 16) const;

    // Is an RVA within a mapped section?
    [[nodiscard]] bool rva_is_mapped(size_t rva) const { return rva < image_.size(); }

private:
    std::vector<uint8_t> image_;   // virtual image, size == SizeOfImage
    uint64_t image_base_ = 0;
    size_t export_dir_rva_ = 0;
    size_t export_dir_size_ = 0;
};

}  // namespace myosotis::pe
