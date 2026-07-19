// PE parser implementation. Standard PE32+ (64-bit) layout.
#include "pe.hpp"
#include "log.hpp"
#include <cstring>
#include <memory>

namespace myosotis::pe {

namespace {

// Locate the NT headers for a PE32+ image. Returns nullptr on any parse issue.
const IMAGE_NT_HEADERS64* nt_headers(const uint8_t* base, size_t size) {
    if (size < sizeof(IMAGE_DOS_HEADER)) return nullptr;
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    if (dos->e_lfanew <= 0 || static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > size)
        return nullptr;
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    return nt;
}

}  // namespace

std::expected<void, std::string> Image::load(const wchar_t* path) {
    // Read the file off disk.
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::unexpected{"CreateFileW failed"};
    auto close = [](HANDLE* p) { if (*p != INVALID_HANDLE_VALUE) CloseHandle(*p); };
    std::unique_ptr<HANDLE, decltype(close)> guard(&h, close);

    LARGE_INTEGER fsz;
    if (!GetFileSizeEx(h, &fsz)) return std::unexpected{"GetFileSizeEx failed"};
    std::vector<uint8_t> file(static_cast<size_t>(fsz.QuadPart));
    DWORD read_total = 0;
    if (!ReadFile(h, file.data(), static_cast<DWORD>(file.size()), &read_total, nullptr))
        return std::unexpected{"ReadFile failed"};
    if (read_total != file.size()) return std::unexpected{"short read"};

    auto nt = nt_headers(file.data(), file.size());
    if (!nt) return std::unexpected{"not a PE32+ image"};
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64)
        return std::unexpected{"not an AMD64 image"};

    const auto& opt = nt->OptionalHeader;
    const size_t size_of_image = opt.SizeOfImage;
    image_base_ = opt.ImageBase;
    image_.assign(size_of_image, 0);

    // Copy headers.
    const size_t headers_size = opt.SizeOfHeaders;
    if (headers_size > file.size() || headers_size > image_.size())
        return std::unexpected{"SizeOfHeaders out of bounds"};
    std::memcpy(image_.data(), file.data(), headers_size);

    // Copy sections: file offset -> RVA in the virtual image.
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        const size_t vaddr = sec->VirtualAddress;
        const size_t raw_size = sec->SizeOfRawData;
        const size_t raw_off = sec->PointerToRawData;
        if (raw_off + raw_size > file.size()) continue;
        if (vaddr + raw_size > image_.size()) continue;
        std::memcpy(image_.data() + vaddr, file.data() + raw_off, raw_size);
    }

    // Locate export directory (if any).
    if (opt.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXPORT) {
        const auto& dir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        export_dir_rva_ = dir.VirtualAddress;
        export_dir_size_ = dir.Size;
    }
    return {};
}

size_t Image::export_rva(const char* name) const {
    if (export_dir_rva_ == 0 || export_dir_rva_ >= image_.size()) return 0;
    auto exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(image_.data() + export_dir_rva_);

    // Bounds-check the three export tables before indexing into them.
    const size_t end = image_.size();
    const auto funcs_off    = exp->AddressOfFunctions;
    const auto names_off    = exp->AddressOfNames;
    const auto ordinals_off = exp->AddressOfNameOrdinals;
    if (funcs_off == 0 || names_off == 0 || ordinals_off == 0) return 0;
    if (static_cast<size_t>(funcs_off)    + sizeof(DWORD) * exp->NumberOfFunctions > end) return 0;
    if (static_cast<size_t>(names_off)    + sizeof(DWORD) * exp->NumberOfNames    > end) return 0;
    if (static_cast<size_t>(ordinals_off) + sizeof(WORD)  * exp->NumberOfNames    > end) return 0;

    const DWORD* funcs   = reinterpret_cast<const DWORD*>(image_.data() + funcs_off);
    const DWORD* names   = reinterpret_cast<const DWORD*>(image_.data() + names_off);
    const WORD*  ordinals = reinterpret_cast<const WORD*>(image_.data() + ordinals_off);
    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        if (names[i] == 0 || names[i] >= image_.size()) continue;
        const char* s = reinterpret_cast<const char*>(image_.data() + names[i]);
        if (std::strcmp(s, name) == 0) {
            return funcs[ordinals[i]];  // RVA
        }
    }
    return 0;
}

std::string Image::read_ascii(size_t rva, size_t max_len) const {
    if (rva >= image_.size()) return {};
    std::string out;
    for (size_t i = 0; i < max_len; ++i) {
        uint8_t b = image_[rva + i];
        if (b == 0) break;
        if (b >= 0x80) break;
        out.push_back(static_cast<char>(b));
    }
    return out;
}

}  // namespace myosotis::pe
