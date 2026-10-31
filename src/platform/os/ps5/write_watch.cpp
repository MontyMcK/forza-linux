// Write watching with page protection: a reset makes written pages read-only;
// the first write faults, and the runtime's SIGSEGV handler calls
// write_watch_on_fault(), which marks the page written and makes it writable
// again (~35 us per fault in a title, FINDINGS 7.5). Kernel writes into a
// read-only page fail (EFAULT) instead of faulting, so callers that let the
// kernel write into game memory call write_watch_before_kernel_write() first.
#include "platform/os/write_watch.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <map>
#include <memory>
#include <shared_mutex>
#include <ctime>

#include "common/log.h"
#include "common/perf.h"
#include "platform/os/memory.h"
#include "platform/os/ps5/kernel.h"
#include "platform/os/thread.h"

namespace fl::os {
using namespace ps5;
namespace {

// Sequential writers (a video decoder filling its frames) fault page after
// page, ~35 us each: 20000 faults a second stalled a movie to ~1 fps. A fault
// where the previous one in the range ended unprotects a run twice as long,
// up to kMaxRun pages, in one call; those pages read as written.
constexpr uint32_t kMaxRun = 64;
struct Sequence {
    std::atomic<uint64_t> next{0};  // where the last run ended
    std::atomic<uint32_t> run{1};   // its length in pages
};

struct Range {
    uint64_t end;
    std::unique_ptr<std::atomic<uint8_t>[]> written;  // one per page
    std::unique_ptr<Sequence> seq = std::make_unique<Sequence>();
};
std::shared_mutex g_mu;
std::map<uint64_t, Range> g_ranges;  // start -> range, never overlapping
std::atomic<uint64_t> g_writer{0};   // thread holding g_mu exclusively (for fault reports)
thread_local int t_readers = 0;      // shared locks of g_mu this thread holds

// A shared lock of g_mu that also counts this thread as a reader.
struct ReadLock {
    std::shared_lock<std::shared_mutex> lock{g_mu};
    ReadLock() { t_readers++; }
    ~ReadLock() { t_readers--; }
};

// The range holding addr, or end(). Caller holds g_mu.
std::map<uint64_t, Range>::iterator range_at(uint64_t addr) {
    auto it = g_ranges.upper_bound(addr);
    if (it == g_ranges.begin()) return g_ranges.end();
    --it;
    return addr < it->second.end ? it : g_ranges.end();
}

// Calls fn(page, range) for every watched page in [start, end); false if a
// page isn't watched. Caller holds g_mu.
// Walks the ranges in order (one map lookup per call, not per page: the
// frame scan covers hundreds of MB and there are thousands of ranges). fn
// returns false to stop early (each_page then returns true).
template <typename Fn>
bool each_page(uint64_t start, uint64_t end, Fn fn) {
    bool all = true;
    uint64_t p = start & ~(kPage - 1);
    auto it = g_ranges.upper_bound(p);
    if (it != g_ranges.begin()) --it;  // the range holding p, or one before it
    while (p < end) {
        while (it != g_ranges.end() && it->second.end <= p) ++it;
        if (it == g_ranges.end() || it->first >= end) return false;  // the rest isn't watched
        if (it->first > p) {
            all = false;
            p = it->first;
        }
        uint64_t stop = std::min(end, it->second.end);
        for (; p < stop; p += kPage)
            if (!fn(p, it->second.written[(p - it->first) / kPage])) return all;
    }
    return all;
}

}  // namespace

bool write_watch_init() { return true; }

bool write_watch_register(uint64_t start, uint64_t end) {
    std::unique_lock lock(g_mu);
    g_writer = thread_id();
    struct Clear { ~Clear() { g_writer = 0; } } clear;
    // Only the parts no range covers yet become ranges (their pages read as
    // written until the first reset); covered pages keep their state. Ranges
    // are never merged: rebuilding a merged range (all of it written and
    // unprotected, under this lock) grew with every one of the thousands of
    // small constant buffers the game places side by side, and stalled every
    // thread for ~750 ms a second during a movie (FINDINGS 7.12).
    for (uint64_t p = start; p < end;) {
        auto it = range_at(p);
        if (it != g_ranges.end()) {
            p = it->second.end;
            continue;
        }
        auto next = g_ranges.upper_bound(p);
        uint64_t gap_end = next == g_ranges.end() ? end : std::min(end, next->first);
        size_t pages = (gap_end - p) / kPage;
        Range r{gap_end, std::make_unique<std::atomic<uint8_t>[]>(pages)};
        for (size_t i = 0; i < pages; i++) r.written[i] = 1;
        g_ranges.emplace(p, std::move(r));
        p = gap_end;
    }
    return true;
}

void write_watch_stats(size_t* ranges, size_t* pages) {
    ReadLock lock;
    *ranges = g_ranges.size();
    *pages = 0;
    for (auto& [start, r] : g_ranges) *pages += (r.end - start) / kPage;
}

void write_watch_unregister(uint64_t start, uint64_t end) {
    std::unique_lock lock(g_mu);
    g_writer = thread_id();
    struct Clear { ~Clear() { g_writer = 0; } } clear;
    auto it = g_ranges.upper_bound(start);
    if (it != g_ranges.begin()) --it;
    while (it != g_ranges.end() && it->first < end) {
        uint64_t rs = it->first, re = it->second.end;
        if (re <= start) {
            ++it;
            continue;
        }
        // Keep the parts outside [start, end), with their written flags.
        Range old = std::move(it->second);
        it = g_ranges.erase(it);
        auto keep = [&](uint64_t a, uint64_t b) {
            if (a >= b) return;
            size_t pages = (b - a) / kPage;
            Range r{b, std::make_unique<std::atomic<uint8_t>[]>(pages)};
            for (size_t i = 0; i < pages; i++) r.written[i] = old.written[(a - rs) / kPage + i].load();
            g_ranges.emplace(a, std::move(r));
        };
        keep(rs, std::min(start, re));
        keep(std::max(end, rs), re);  // past end: the loop stops there
    }
}

bool write_watch_reset(uint64_t start, uint64_t end, bool strict, WrittenFn on_written, void* user, size_t* calls) {
    ReadLock lock;
    uint64_t run = 0, run_end = 0;
    size_t n_calls = 0;
    auto flush = [&] {
        if (run_end > run) {
            if (on_written) on_written(run, run_end, user);
            protect(reinterpret_cast<void*>(run), run_end - run, kRead);
            n_calls++;
        }
        run = run_end = 0;
    };
    bool all = each_page(start, end, [&](uint64_t p, std::atomic<uint8_t>& w) {
        // Clear first, then protect: a write in between lands before the
        // caller's copy; later writes fault and mark the page again.
        if (!w.exchange(0)) return true;
        if (run_end != p) flush();
        if (!run_end) run = p;
        run_end = p + kPage;
        return true;
    });
    flush();
    if (calls) *calls = n_calls;
    if (strict && !all) {
        errno = EPERM;
        return false;
    }
    return true;
}

bool write_watch_written(uint64_t start, uint64_t end) {
    ReadLock lock;
    bool any = false;
    bool all = each_page(start, end, [&](uint64_t, std::atomic<uint8_t>& w) {
        any = w.load(std::memory_order_relaxed) != 0;
        return !any;  // the first written page answers it
    });
    return any || !all;
}

bool write_watch_on_fault(uint64_t addr, bool) {
    // Watched pages stay readable, so a fault in a watched range is a write
    // (no need for the error-code bit, whose offset wasn't measured).
    // A thread that faults while it holds g_mu (or while a writer waits and
    // it already reads) would wait for itself: give up after 2 s and let the
    // crash report say where it faulted.
    std::shared_lock lock(g_mu, std::defer_lock);
    for (int ms = 0; !lock.try_lock(); ms++) {
        if (ms == 2000) {
            FL_ERROR("os: write watch: fault at %p while its lock is held (writer thread %llu, this thread %llu, "
                     "reading %d)", reinterpret_cast<void*>(addr), (unsigned long long)g_writer.load(),
                     (unsigned long long)thread_id(), t_readers);
            return false;
        }
        timespec t{0, 1000000};
        nanosleep(&t, nullptr);
    }
    auto it = range_at(addr);
    if (it == g_ranges.end()) return false;
    perf::add(perf::kWriteFaults);
    Range& r = it->second;
    uint64_t p = addr & ~(kPage - 1);
    uint32_t n = p == r.seq->next.load(std::memory_order_relaxed)
                     ? std::min(r.seq->run.load(std::memory_order_relaxed) * 2, kMaxRun)
                     : 1;
    uint64_t end = std::min(p + n * kPage, r.end);
    for (uint64_t q = p; q < end; q += kPage) r.written[(q - it->first) / kPage] = 1;
    r.seq->run.store(n, std::memory_order_relaxed);
    r.seq->next.store(end, std::memory_order_relaxed);
    return protect(reinterpret_cast<void*>(p), end - p, kRead | kWrite);
}

void write_watch_before_kernel_write(void* addr, size_t size) {
    uint64_t a = reinterpret_cast<uint64_t>(addr);
    ps5::write_watch_unprotect(a & ~(kPage - 1), (a + size + kPage - 1) & ~(kPage - 1));
}

void ps5::write_watch_unprotect(uint64_t start, uint64_t end) {
    ReadLock lock;
    each_page(start & ~(kPage - 1), end, [&](uint64_t p, std::atomic<uint8_t>& w) {
        if (!w.exchange(1)) protect(reinterpret_cast<void*>(p), kPage, kRead | kWrite);
        return true;
    });
}

bool pages_mapped(uint64_t start, uint64_t end) {
    for (uint64_t p = start; p < end;) {
        void *s, *e;
        uint32_t prot;
        if (sceKernelQueryMemoryProtection(reinterpret_cast<void*>(p), &s, &e, &prot) != 0) return false;
        p = reinterpret_cast<uint64_t>(e);
    }
    return true;
}

bool pages_readable(uint64_t start, uint64_t end) {
    for (uint64_t p = start; p < end;) {
        void *s, *e;
        uint32_t prot;
        if (sceKernelQueryMemoryProtection(reinterpret_cast<void*>(p), &s, &e, &prot) != 0 || !(prot & kRead)) return false;
        p = reinterpret_cast<uint64_t>(e);
    }
    return true;
}

// One page: the game's constant buffers are ~0x60 bytes, and an mprotect costs
// ~28 us here (FINDINGS 7.13) against a few ns to compare them.
size_t write_watch_compare_below() { return kPage; }

}  // namespace fl::os
