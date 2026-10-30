// A fast monotonic clock. clock_gettime costs ~15 ns on Linux but ~880 ns in
// a PS5 title (FINDINGS 7.13), and the game reads its timer constantly.
#pragma once

#include <cstdint>

namespace fl::os {

// Nanoseconds on CLOCK_MONOTONIC's scale (comparable with clock_gettime).
uint64_t monotonic_ns();

}  // namespace fl::os
