// The TSC (sceKernelReadTsc), anchored to CLOCK_MONOTONIC at the first call.
#include "platform/os/clock.h"

#include <ctime>

extern "C" {
uint64_t sceKernelReadTsc(void);
uint64_t sceKernelGetTscFrequency(void);
}

namespace fl::os {

uint64_t monotonic_ns() {
    static const struct Anchor {
        uint64_t tsc, ns, hz;
        Anchor() {
            timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            tsc = sceKernelReadTsc();
            ns = uint64_t(ts.tv_sec) * 1'000'000'000ull + uint64_t(ts.tv_nsec);
            hz = sceKernelGetTscFrequency();
        }
    } a;
    return a.ns + uint64_t((unsigned __int128)(sceKernelReadTsc() - a.tsc) * 1'000'000'000u / a.hz);
}

}  // namespace fl::os
