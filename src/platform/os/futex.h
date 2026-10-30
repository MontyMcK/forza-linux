// Wait on / wake a 32-bit word, private to this process (Linux futex, PS5 _umtx_op).
#pragma once

#include <cstdint>

namespace fl::os {

// Sleeps while *addr == expected, until a wake, a spurious wake-up or
// timeout_ns (relative; -1 = none) passes. Returns false only on timeout.
bool futex_wait(void* addr, int32_t expected, int64_t timeout_ns = -1);

// Wakes up to `count` threads waiting on addr.
void futex_wake(void* addr, int32_t count);

}  // namespace fl::os
