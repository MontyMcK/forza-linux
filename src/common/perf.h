// Event counters for FL_LOG=fps: a frame over 50 ms logs what changed during
// it (dxgi.cpp frame_stats). Relaxed atomic adds: cheap enough to keep on.
#pragma once

#include <atomic>
#include <cstdint>

#include "platform/os/clock.h"

namespace fl::perf {

enum Counter : int {
    kCommits, kCommitNs,          // VirtualAlloc commits and new reservations
    kFrees, kFreeNs,              // VirtualFree decommits and releases
    kFileReads, kFileReadBytes, kFileReadNs,
    kFileOpens, kFileOpenNs,      // CreateFile and descriptor-pool reopens
    kWriteFaults,                 // writes to watched pages (PS5)
    kUploads,                     // placement uploads
    kThreads,                     // CreateThread
    kCount
};

inline std::atomic<uint64_t> g_counters[kCount];

inline void add(Counter c, uint64_t n = 1) { g_counters[c].fetch_add(n, std::memory_order_relaxed); }

// Counts one event and adds the time until it goes out of scope.
struct Timed {
    Counter count, ns;
    uint64_t t0 = os::monotonic_ns();
    Timed(Counter c, Counter n) : count(c), ns(n) {}
    ~Timed() {
        add(count);
        add(ns, os::monotonic_ns() - t0);
    }
};

}  // namespace fl::perf
