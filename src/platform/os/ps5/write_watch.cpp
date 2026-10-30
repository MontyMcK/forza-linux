// Write watching with page protection: a reset makes written pages read-only;
// the first write faults, and the runtime's SIGSEGV handler calls
// write_watch_on_fault(), which marks the page written and makes it writable
// again (~35 us per fault in a title, FINDINGS 7.5). Kernel writes into a
// read-only page fail (EFAULT) instead of faulting, so callers that let the
// kernel write into game memory call write_watch_before_kernel_write() first.
#include "platform/os/write_watch.h"

#include <atomic>
#include <cerrno>
#include <map>
#include <memory>
#include <shared_mutex>
#include <ctime>

#include "common/log.h"
#include "platform/os/memory.h"
#include "platform/os/ps5/kernel.h"
#include "platform/os/thread.h"

namespace fl::os {
using namespace ps5;
namespace {

struct Range {
    uint64_t end;
    std::unique_ptr<std::atomic<uint8_t>[]> written;  // one per page
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
template <typename Fn>
bool each_page(uint64_t start, uint64_t end, Fn fn) {
    bool all = true;
    for (uint64_t p = start; p < end; p += kPage) {
        auto it = range_at(p);
        if (it == g_ranges.end()) {
            all = false;
            continue;
        }
        fn(p, it->second.written[(p - it->first) / kPage]);
    }
    return all;
}

}  // namespace

bool write_watch_init() { return true; }

bool write_watch_register(uint64_t start, uint64_t end) {
    std::unique_lock lock(g_mu);
    g_writer = thread_id();
    struct Clear { ~Clear() { g_writer = 0; } } clear;
    auto it = range_at(start);
    if (it != g_ranges.end()) {
        if (it->first == start && it->second.end == end) return true;
        start = it->first;  // the range holding start: the loop below merges it
    }
    // Merge with every range it touches; everything in the result reads as written.
    for (auto o = g_ranges.lower_bound(start > kPage ? start - kPage : 0); o != g_ranges.end() && o->first <= end;) {
        if (o->second.end < start) { ++o; continue; }
        start = std::min(start, o->first);
        end = std::max(end, o->second.end);
        o = g_ranges.erase(o);
    }
    size_t pages = (end - start) / kPage;
    Range r{end, std::make_unique<std::atomic<uint8_t>[]>(pages)};
    for (size_t i = 0; i < pages; i++) r.written[i] = 1;
    // Pages a reset protected become writable again: they all read as written now.
    protect(reinterpret_cast<void*>(start), end - start, kRead | kWrite);
    g_ranges.emplace(start, std::move(r));
    return true;
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
        if (!w.exchange(0)) return;
        if (run_end != p) flush();
        if (!run_end) run = p;
        run_end = p + kPage;
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
    bool all = each_page(start, end, [&](uint64_t, std::atomic<uint8_t>& w) { any |= w.load() != 0; });
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
    uint64_t p = addr & ~(kPage - 1);
    it->second.written[(p - it->first) / kPage] = 1;
    return protect(reinterpret_cast<void*>(p), kPage, kRead | kWrite);
}

void write_watch_before_kernel_write(void* addr, size_t size) {
    uint64_t a = reinterpret_cast<uint64_t>(addr);
    ps5::write_watch_unprotect(a & ~(kPage - 1), (a + size + kPage - 1) & ~(kPage - 1));
}

void ps5::write_watch_unprotect(uint64_t start, uint64_t end) {
    ReadLock lock;
    each_page(start & ~(kPage - 1), end, [&](uint64_t p, std::atomic<uint8_t>& w) {
        if (!w.exchange(1)) protect(reinterpret_cast<void*>(p), kPage, kRead | kWrite);
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

}  // namespace fl::os
