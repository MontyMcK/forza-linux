#pragma once

#include <atomic>
#include <string>

#include "loader/pe_image.h"

namespace fl::loader {

struct ImportInfo {
    void* target;  // must stay first (thunks.S jumps through it)
    std::string dll;
    std::string name;
    std::atomic<uint64_t> calls{0};
};

// Resolve every import of `img`, writing the IAT. Unknown imports are bound to
// traps that abort with "UNIMPLEMENTED dll!name". Returns the number of
// unresolved imports.
int resolve_imports(PeImage& img);

// Resolve a dynamically requested export (GetProcAddress on an HLE module).
// Returns nullptr if unknown.
void* resolve_dynamic(const std::string& dll, const std::string& name);

// A code stub that aborts with "UNIMPLEMENTED <what>!<name>" when called.
// Used for unimplemented COM/WinRT vtable slots too.
void* make_named_trap(const std::string& what, const std::string& name);

}  // namespace fl::loader
