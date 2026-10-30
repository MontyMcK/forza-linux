// Random bytes from the host (BCryptGenRandom).
#pragma once

#include <cstddef>

namespace fl::os {

void random_bytes(void* buf, size_t len);

}  // namespace fl::os
