#include "platform/os/futex.h"

#include <cerrno>
#include <ctime>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace fl::os {

bool futex_wait(void* addr, int32_t expected, int64_t timeout_ns) {
    timespec ts, *tp = nullptr;
    if (timeout_ns >= 0) {
        ts = {static_cast<time_t>(timeout_ns / 1000000000), static_cast<long>(timeout_ns % 1000000000)};
        tp = &ts;
    }
    long r = syscall(SYS_futex, static_cast<int32_t*>(addr), FUTEX_WAIT_PRIVATE, expected, tp, nullptr, 0);
    return !(r != 0 && errno == ETIMEDOUT);
}

void futex_wake(void* addr, int32_t count) {
    syscall(SYS_futex, static_cast<int32_t*>(addr), FUTEX_WAKE_PRIVATE, count, nullptr, nullptr, 0);
}

}  // namespace fl::os
