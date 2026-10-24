// Minimal PNG writer (RGBA8), for screenshots.
#pragma once

#include <cstdint>
#include <string>

namespace fl {

// `rgba` is width*height*4 bytes, rows top to bottom. Returns false on I/O error.
bool write_png(const std::string& path, const uint8_t* rgba, int width, int height);

}  // namespace fl
