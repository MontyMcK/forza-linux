#include "platform/os/futex.h"

#include <cerrno>
#include <ctime>

#include "platform/os/ps5/kernel.h"

namespace fl::os {
namespace {
constexpr int kWaitUintPrivate = 15, kWakePrivate = 16;  // FreeBSD UMTX_OP_*, FINDINGS 7.3
}

bool futex_wait(void* addr, int32_t expected, int64_t timeout_ns) {
    timespec ts, *tp = nullptr;
    if (timeout_ns >= 0) {
        ts = {static_cast<time_t>(timeout_ns / 1000000000), static_cast<long>(timeout_ns % 1000000000)};
        tp = &ts;
    }
    // uaddr 0: uaddr2 is a relative timeout.
    int r = _umtx_op(addr, kWaitUintPrivate, static_cast<uint32_t>(expected), nullptr, tp);
    return !(r != 0 && errno == ETIMEDOUT);
}

void futex_wake(void* addr, int32_t count) { _umtx_op(addr, kWakePrivate, static_cast<unsigned long>(count), nullptr, nullptr); }

}  // namespace fl::os
