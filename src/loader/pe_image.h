#pragma once

#include <cstdint>
#include <string>

namespace fl::loader {

struct PeImage {
    std::string path;
    uint8_t* base = nullptr;        // where it is mapped
    uint64_t preferred_base = 0;    // ImageBase from the header
    uint32_t size = 0;              // SizeOfImage
    uint32_t entry_rva = 0;
    uint32_t tls_dir_rva = 0;
    uint32_t import_dir_rva = 0;
    uint32_t import_dir_size = 0;
    uint32_t exception_dir_rva = 0;  // .pdata (RUNTIME_FUNCTION[])
    uint32_t exception_dir_size = 0;
    uint64_t stack_reserve = 0;

    template <typename T>
    T* at(uint32_t rva) const { return reinterpret_cast<T*>(base + rva); }
    uint64_t rva_of(const void* p) const { return reinterpret_cast<const uint8_t*>(p) - base; }
    bool contains(const void* p) const {
        auto* b = reinterpret_cast<const uint8_t*>(p);
        return b >= base && b < base + size;
    }
};

// Map the image (headers + sections), apply relocations, set protections.
// Does not resolve imports. Aborts on failure.
PeImage map_image(const std::string& path);

// Re-apply final page protections after imports are written.
void protect_image(PeImage& img);

// The main executable image, once loaded.
PeImage& main_image();

}  // namespace fl::loader
