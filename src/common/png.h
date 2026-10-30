// Minimal PNG writer (RGBA8), for screenshots and the game's WIC PNG encodes.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fl {

// `rgba` is width*height*4 bytes, rows top to bottom. Returns false on I/O error.
bool write_png(const std::string& path, const uint8_t* rgba, int width, int height);
// The same PNG in memory (empty on failure); rows are `stride` bytes apart.
std::vector<uint8_t> encode_png(const uint8_t* rgba, int width, int height, size_t stride);

}  // namespace fl
