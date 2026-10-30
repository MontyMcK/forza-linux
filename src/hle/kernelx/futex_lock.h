// Small futex-based lock that lives inside game-owned memory
// (CRITICAL_SECTION, SRWLOCK). 0 = free, 1 = locked, 2 = locked + waiters.
// Zero-initialized memory is a valid unlocked lock.
#pragma once

#include <atomic>
#include <cstdint>

#include "platform/os/futex.h"

namespace fl::kx {

inline void futex_wait(std::atomic<int32_t>* addr, int32_t expected) {
    os::futex_wait(addr, expected);
}

inline void futex_wake(std::atomic<int32_t>* addr, int32_t n) {
    os::futex_wake(addr, n);
}

inline bool futex_try_lock(std::atomic<int32_t>* s) {
    int32_t c = 0;
    return s->compare_exchange_strong(c, 1, std::memory_order_acquire);
}

inline void futex_lock(std::atomic<int32_t>* s, uint32_t spin = 0) {
    for (uint32_t i = 0; i < spin; i++) {
        if (futex_try_lock(s)) return;
        __builtin_ia32_pause();
    }
    int32_t c = 0;
    if (s->compare_exchange_strong(c, 1, std::memory_order_acquire)) return;
    if (c != 2) c = s->exchange(2, std::memory_order_acquire);
    while (c != 0) {
        futex_wait(s, 2);
        c = s->exchange(2, std::memory_order_acquire);
    }
}

inline void futex_unlock(std::atomic<int32_t>* s) {
    if (s->fetch_sub(1, std::memory_order_release) != 1) {
        s->store(0, std::memory_order_release);
        futex_wake(s, 1);
    }
}

}  // namespace fl::kx
