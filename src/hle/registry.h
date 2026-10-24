// Registry of HLE functions the loader can bind game imports to.
//
// Usage in any .cpp under src/hle/:
//
//   WINAPI DWORD GetTickCount_() { ... }
//   HLE_EXPORT(kernelx, GetTickCount, GetTickCount_);
//   HLE_EXPORT_AS(msvcr110, "?terminate@@YAXXZ", terminate_);   // mangled names
//
// DLL names are matched case-insensitively and without the ".dll" suffix.
#pragma once

#include <string>
#include <string_view>

namespace fl::hle {

// Returns nullptr if (dll, name) has no HLE implementation.
void* lookup(std::string_view dll, std::string_view name);

struct Registrar {
    Registrar(const char* dll, const char* name, void* fn);
};

// Normalize "KERNELX.DLL" -> "kernelx".
std::string normalize_dll(std::string_view dll);

// Number of registered exports (for the startup log).
size_t count();

}  // namespace fl::hle

#define FL_HLE_CAT2(a, b) a##b
#define FL_HLE_CAT(a, b) FL_HLE_CAT2(a, b)

#define HLE_EXPORT_AS(dll, name, fn) \
    static ::fl::hle::Registrar FL_HLE_CAT(fl_hle_reg_, __COUNTER__)(#dll, name, reinterpret_cast<void*>(&fn))

#define HLE_EXPORT(dll, name, fn) HLE_EXPORT_AS(dll, #name, fn)
