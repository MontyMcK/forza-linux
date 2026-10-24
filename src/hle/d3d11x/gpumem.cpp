#include "hle/d3d11x/gpumem.h"

#include <atomic>
#include <mutex>
#include <sys/mman.h>

#include "common/log.h"
#include "loader/crash.h"

namespace fl::d3d::gpumem {
namespace {

constexpr int kPageShift = 12;
constexpr uint64_t kPage = 1ull << kPageShift;
constexpr int kChunkShift = 18;                    // pages per chunk: 2^18 (1 GB)
constexpr uint64_t kChunks = 1ull << (47 - kPageShift - kChunkShift);

enum : uint8_t { TRACKED = 1, PROTECTED = 2, DIRTY = 4 };

// Two-level table of per-page states. Chunks are allocated by track() (normal
// context) and never freed, so the fault hook can read them without locks.
std::atomic<std::atomic<uint8_t>*> g_chunks[kChunks];
std::mutex g_alloc_mutex;

std::atomic<uint8_t>* state(uint64_t page) {
    uint64_t c = page >> kChunkShift;
    if (c >= kChunks) return nullptr;
    std::atomic<uint8_t>* chunk = g_chunks[c].load(std::memory_order_acquire);
    return chunk ? &chunk[page & ((1u << kChunkShift) - 1)] : nullptr;
}

std::atomic<uint8_t>* state_alloc(uint64_t page) {
    uint64_t c = page >> kChunkShift;
    if (c >= kChunks) fatal("gpumem: address out of range");
    if (!g_chunks[c].load(std::memory_order_acquire)) {
        std::lock_guard lock(g_alloc_mutex);
        if (!g_chunks[c].load()) g_chunks[c].store(new std::atomic<uint8_t>[1u << kChunkShift]());
    }
    return state(page);
}

void* page_ptr(uint64_t page) { return reinterpret_cast<void*>(page << kPageShift); }

// Async-signal-safe: atomics and mprotect only.
bool on_fault(uint64_t addr, bool write) {
    if (!write) return false;
    uint64_t page = addr >> kPageShift;
    std::atomic<uint8_t>* s = state(page);
    if (!s) return false;
    uint8_t v = s->load(std::memory_order_acquire);
    if (!(v & TRACKED)) return false;
    // Two threads writing one protected page both fault; the second finds it
    // already unprotected and must just retry. A second fault at the same
    // address on the same thread means the fault is genuine: let it crash.
    static thread_local uint64_t last_retry = 0;
    if (!(v & PROTECTED)) {
        if (last_retry == addr) {
            last_retry = 0;
            return false;
        }
        last_retry = addr;
        return true;
    }
    last_retry = 0;
    if (mprotect(page_ptr(page), kPage, PROT_READ | PROT_WRITE) != 0) return false;
    s->fetch_or(DIRTY, std::memory_order_acq_rel);
    s->fetch_and(static_cast<uint8_t>(~PROTECTED), std::memory_order_acq_rel);
    return true;
}

void range_pages(const void* addr, size_t size, uint64_t* first, uint64_t* last) {
    uint64_t a = reinterpret_cast<uint64_t>(addr);
    *first = a >> kPageShift;
    *last = (a + (size ? size : 1) - 1) >> kPageShift;
}

}  // namespace

void init() { loader::set_fault_hook(&on_fault); }

void track(const void* addr, size_t size) {
    uint64_t first, last;
    range_pages(addr, size, &first, &last);
    for (uint64_t p = first; p <= last; p++) state_alloc(p)->fetch_or(TRACKED | DIRTY);
}

bool dirty(const void* addr, size_t size) {
    uint64_t first, last;
    range_pages(addr, size, &first, &last);
    for (uint64_t p = first; p <= last; p++) {
        std::atomic<uint8_t>* s = state(p);
        if (!s || (s->load(std::memory_order_acquire) & (TRACKED | DIRTY)) != (TRACKED | DIRTY)) continue;
        return true;
    }
    return false;
}

void begin_upload(const void* addr, size_t size) {
    uint64_t first, last;
    range_pages(addr, size, &first, &last);
    // 1. Mark protected, 2. protect, 3. clear DIRTY on pages that are still protected.
    for (uint64_t p = first; p <= last; p++) state_alloc(p)->fetch_or(PROTECTED);
    if (mprotect(page_ptr(first), (last - first + 1) * kPage, PROT_READ) != 0)
        fatal("gpumem: mprotect(%p, %llu pages) failed", page_ptr(first), (unsigned long long)(last - first + 1));
    for (uint64_t p = first; p <= last; p++) {
        std::atomic<uint8_t>* s = state(p);
        uint8_t expect = TRACKED | PROTECTED | DIRTY;
        s->compare_exchange_strong(expect, TRACKED | PROTECTED);
    }
}

void prepare_write(void* addr, size_t size) {
    if (!size) return;
    uint64_t first, last;
    range_pages(addr, size, &first, &last);
    for (uint64_t p = first; p <= last; p++) {
        std::atomic<uint8_t>* s = state(p);
        if (!s || !(s->load() & PROTECTED)) continue;
        mprotect(page_ptr(p), kPage, PROT_READ | PROT_WRITE);
        s->fetch_or(DIRTY);
        s->fetch_and(static_cast<uint8_t>(~PROTECTED));
    }
}

}  // namespace fl::d3d::gpumem
