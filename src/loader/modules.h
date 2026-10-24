// PE DLLs from the game package (Microsoft.Xbox.Services.dll, ...), loaded
// like the main executable: mapped, relocated, imports bound to our HLE, then
// DllMain. They are the game's own shipped dependencies (Mount/), not system
// DLLs (CLAUDE.md Rule 1 covers EmbeddedXvd/).
#pragma once

#include <string>
#include <vector>

#include "loader/pe_image.h"

namespace fl::loader {

// Loads (once) and returns the module; nullptr if the file doesn't exist.
// Must be called on a game thread (DllMain runs on it).
PeImage* load_pe_module(const std::string& path);

// The image (main exe or a loaded module) containing addr, or nullptr.
// Lock-free: safe from signal handlers and the unwinder.
PeImage* image_for(const void* addr);

// Exported symbol by name, or "#<ordinal>". nullptr if missing.
void* pe_export(const PeImage& img, const std::string& name);

// The loaded module whose base is `base` (an HMODULE), or nullptr.
PeImage* module_by_base(const void* base);

}  // namespace fl::loader
