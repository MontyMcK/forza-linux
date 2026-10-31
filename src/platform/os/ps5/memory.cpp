// PS5 virtual memory. Address space comes from sceKernelReserveVirtualRange;
// committed memory is direct memory (a title has ~448 MiB of flexible memory
// and 12 GiB of direct memory, FINDINGS 7.5), mapped over the reservation in
// 64 KiB units. Execute is added with sceKernelMprotect after mapping (the
// kernel refuses it at map time). MAP_FIXED|MAP_EXCL replaces mappings here,
// so exact addresses are checked by reserving at a hint and comparing.
#include "platform/os/memory.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

#include "common/log.h"
#include "platform/os/ps5/kernel.h"

namespace fl::os {
using namespace ps5;
namespace {

std::mutex g_mu;
struct Block {
    int64_t phys;
    size_t len;
};
std::map<uint64_t, Block> g_blocks;  // committed direct memory: start -> block (unit aligned)
std::atomic<uint64_t> g_committed{0};

uint64_t round_up(uint64_t n, uint64_t a) { return (n + a - 1) & ~(a - 1); }

// The block holding unit address u, or g_blocks.end(). Caller holds g_mu.
std::map<uint64_t, Block>::iterator block_at(uint64_t u) {
    auto it = g_blocks.upper_bound(u);
    if (it == g_blocks.begin()) return g_blocks.end();
    --it;
    return u < it->first + it->second.len ? it : g_blocks.end();
}

// Backs [rs, re) (unit aligned, not committed yet) with zeroed direct memory.
bool back_run(uint64_t rs, uint64_t re) {
    size_t len = re - rs;
    int64_t phys = 0;
    if (int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), len, kUnit, kDirectTypeCpu, &phys)) {
        FL_ERROR("os: commit %p+0x%zx: sceKernelAllocateDirectMemory failed (0x%x)", reinterpret_cast<void*>(rs), len, rc);
        errno = ENOMEM;
        return false;
    }
    void* va = reinterpret_cast<void*>(rs);
    int rc = sceKernelMapDirectMemory(&va, len, kRead | kWrite, kMapFixed, phys, kUnit);
    if (rc != 0 || va != reinterpret_cast<void*>(rs)) {
        void *s = nullptr, *e = nullptr;
        uint32_t prot = 0;
        int q = sceKernelQueryMemoryProtection(reinterpret_cast<void*>(rs), &s, &e, &prot);
        FL_ERROR("os: commit %p+0x%zx: sceKernelMapDirectMemory(phys 0x%llx) -> 0x%x at %p (here: query 0x%x, %p-%p prot 0x%x)",
                 reinterpret_cast<void*>(rs), len, (unsigned long long)phys, rc, va, q, s, e, prot);
        if (rc == 0) sceKernelMunmap(va, len);
        sceKernelReleaseDirectMemory(phys, len);
        errno = ENOMEM;
        return false;
    }
    memset(va, 0, len);  // released direct memory comes back with its old contents
    g_blocks[rs] = {phys, len};
    g_committed += len;
    return true;
}

// Bytes from p (up to n) whose pages allow `need` access.
size_t accessible(const void* p, size_t n, uint32_t need) {
    uint64_t a = reinterpret_cast<uint64_t>(p), end = a + n, ok = a;
    while (ok < end) {
        void *s = nullptr, *e = nullptr;
        uint32_t prot = 0;
        if (sceKernelQueryMemoryProtection(reinterpret_cast<void*>(ok), &s, &e, &prot) != 0 || (prot & need) != need) break;
        ok = std::min<uint64_t>(end, reinterpret_cast<uint64_t>(e));
    }
    return ok - a;
}

// Where reservations without an address go: from 64 GiB up to the 1 TiB a
// title may use (16 GiB ranges land exactly at their hint there, FINDINGS
// 7.2). Left to itself the kernel fills the low areas, including the GPU
// window (0x2_0000_0000-0x2_FFFF_FFFF) and ranges the game asks for by
// address.
constexpr uint64_t kAnywhereLo = 0x1000000000ull, kAnywhereHi = 0x10000000000ull;
std::atomic<uint64_t> g_next_anywhere{kAnywhereLo};

void* reserve_anywhere(size_t size, size_t align) {
    align = std::max(align, kUnit);
    // From where the last one ended, then once more from the bottom, so freed
    // ranges are used again: only climbing would run out of the title's 1 TiB
    // in a long session (large heap blocks come and go all the time).
    uint64_t from = round_up(g_next_anywhere.load(), align);
    for (int pass = 0; pass < 2; pass++) {
        uint64_t stop = pass ? from : kAnywhereHi;
        for (uint64_t hint = pass ? kAnywhereLo : from; hint < stop && hint + size <= kAnywhereHi;) {
            void* a = reinterpret_cast<void*>(hint);
            if (sceKernelReserveVirtualRange(&a, size, 0, align) != 0) return nullptr;
            uint64_t got = reinterpret_cast<uint64_t>(a);
            if (got >= kAnywhereLo && got + size <= kAnywhereHi) {
                g_next_anywhere = got + size;
                return a;
            }
            sceKernelMunmap(a, size);  // landed low: try further up
            hint += std::max<uint64_t>(size, 1ull << 30);
        }
    }
    return nullptr;
}

// Gives the direct memory of whole units [a, b) back; the range stays
// reserved. A fixed reservation replaces the mapping in one call, so nothing
// else can land in between. Caller holds g_mu.
void unback(uint64_t a, uint64_t b) {
    auto it = g_blocks.upper_bound(a);
    if (it != g_blocks.begin()) --it;
    while (it != g_blocks.end() && it->first < b) {
        uint64_t s = it->first, e = s + it->second.len;
        if (e <= a) {
            ++it;
            continue;
        }
        uint64_t x = std::max(a, s), y = std::min(b, e);
        Block blk = it->second;
        void* va = reinterpret_cast<void*>(x);
        if (sceKernelReserveVirtualRange(&va, y - x, kMapFixed, kPage) != 0 || va != reinterpret_cast<void*>(x)) {
            // Fall back to unmapping first (a short window without a reservation).
            sceKernelMunmap(reinterpret_cast<void*>(x), y - x);
            va = reinterpret_cast<void*>(x);
            if (sceKernelReserveVirtualRange(&va, y - x, kMapFixed, kPage) != 0 || va != reinterpret_cast<void*>(x))
                FL_ERROR("os: decommit %p+0x%llx: could not reserve the range again", reinterpret_cast<void*>(x),
                         (unsigned long long)(y - x));
        }
        sceKernelReleaseDirectMemory(blk.phys + int64_t(x - s), y - x);
        g_committed -= y - x;
        it = g_blocks.erase(it);
        if (s < x) g_blocks[s] = {blk.phys, x - s};
        if (y < e) g_blocks[y] = {blk.phys + int64_t(y - s), e - y};  // y == b: past the range
    }
}

// Whether direct memory maps at unit address `at`, inside a reservation of
// ours. A title can't map it everywhere it can reserve: 0x917440000 gave
// EINVAL (FINDINGS 7.8). A refused map leaves the reservation as it was; a
// successful test map is undone and the unit reserved again.
bool direct_mappable(uint64_t at) {
    int64_t phys = 0;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), kUnit, kUnit, kDirectTypeCpu, &phys) != 0)
        return true;  // can't tell now: commit() reports it if it fails
    void* va = reinterpret_cast<void*>(at);
    int rc = sceKernelMapDirectMemory(&va, kUnit, kRead | kWrite, kMapFixed, phys, kUnit);
    bool here = rc == 0 && va == reinterpret_cast<void*>(at);
    if (rc == 0) sceKernelMunmap(va, kUnit);
    sceKernelReleaseDirectMemory(phys, kUnit);
    if (!here) return false;
    void* a = reinterpret_cast<void*>(at);
    rc = sceKernelReserveVirtualRange(&a, kUnit, 0, kPage);
    if (rc != 0 || a != reinterpret_cast<void*>(at)) {
        FL_ERROR("os: lost the reservation at %p after a test map", reinterpret_cast<void*>(at));
        if (rc == 0) sceKernelMunmap(a, kUnit);
        return false;
    }
    return true;
}

}  // namespace

size_t page_size() { return kPage; }

void* map_anon(void* want, size_t size, unsigned prot, size_t align) {
    // Whole units, so two reservations never share one (commit() backs units).
    size = round_up(size ? size : 1, kUnit);
    void* a = want;
    if (!want) {
        a = reserve_anywhere(size, align);
        if (!a) {
            errno = ENOMEM;
            return nullptr;
        }
    } else if (sceKernelReserveVirtualRange(&a, size, 0, kPage) != 0) {
        errno = ENOMEM;
        return nullptr;
    }
    if (want && a != want) {  // the kernel moved it: something is mapped there
        sceKernelMunmap(a, size);
        errno = EEXIST;
        return nullptr;
    }
    if (want) {
        uint64_t first = reinterpret_cast<uint64_t>(want), last = first + size - kUnit;
        if (!direct_mappable(first) || (last != first && !direct_mappable(last))) {
            FL_LOG(memory, "os: %p+0x%zx can be reserved but not backed here", want, size);
            sceKernelMunmap(a, size);
            errno = EINVAL;
            return nullptr;
        }
    }
    if (prot != kNone && !commit(a, size, prot)) {
        release(a, size);
        return nullptr;
    }
    return a;
}

bool commit(void* addr, size_t size, unsigned prot) {
    uint64_t lo = reinterpret_cast<uint64_t>(addr) & ~(kPage - 1);
    uint64_t hi = round_up(reinterpret_cast<uint64_t>(addr) + size, kPage);
    uint64_t ulo = lo & ~(kUnit - 1), uhi = round_up(hi, kUnit);
    std::lock_guard lock(g_mu);
    bool fresh = true;  // every unit backed by this call (mapped read/write)
    for (uint64_t u = ulo; u < uhi;) {
        if (block_at(u) != g_blocks.end()) {
            fresh = false;
            u += kUnit;
            continue;
        }
        uint64_t re = u;
        while (re < uhi && block_at(re) == g_blocks.end()) re += kUnit;
        if (!back_run(u, re)) {
            int64_t phys = 0;
            size_t largest = 0;
            sceKernelAvailableDirectMemorySize(0, sceKernelGetDirectMemorySize(), kUnit, &phys, &largest);
            FL_ERROR("os: commit %p+0x%llx failed (we hold %llu MiB; largest free block %zu MiB of %lld MiB)",
                     reinterpret_cast<void*>(u), (unsigned long long)(re - u), (unsigned long long)(g_committed.load() >> 20),
                     largest >> 20, (long long)(sceKernelGetDirectMemorySize() >> 20));
            return false;
        }
        // Pages of the new units outside the request stay inaccessible (reserved).
        if (u < lo) sceKernelMprotect(reinterpret_cast<void*>(u), lo - u, kNone);
        if (re > hi) sceKernelMprotect(reinterpret_cast<void*>(hi), re - hi, kNone);
        u = re;
    }
    // An mprotect costs ~28 us here (FINDINGS 7.13): skip it when back_run
    // already mapped exactly this, read/write.
    if (fresh && lo == ulo && hi == uhi && prot == (kRead | kWrite)) return true;
    return sceKernelMprotect(reinterpret_cast<void*>(lo), hi - lo, prot) == 0;
}

bool decommit(void* addr, size_t size) {
    // Whole 64 KiB units give their direct memory back (the game streams by
    // decommitting, as the Xbox returns that memory). Pages of a unit that
    // stays partly committed are zeroed and made inaccessible, one call per
    // run: page by page cost two mprotects (~57 us) per 16 KiB.
    uint64_t lo = reinterpret_cast<uint64_t>(addr) & ~(kPage - 1);
    uint64_t hi = round_up(reinterpret_cast<uint64_t>(addr) + size, kPage);
    std::lock_guard lock(g_mu);
    uint64_t whole_lo = 0, whole_hi = 0;  // a run of whole units, unbacked together
    for (uint64_t u = lo & ~(kUnit - 1); u < hi; u += kUnit) {
        uint64_t x = std::max(lo, u), y = std::min(hi, u + kUnit);
        if (x == u && y == u + kUnit) {
            if (whole_hi != u) {
                if (whole_hi > whole_lo) unback(whole_lo, whole_hi);
                whole_lo = u;
            }
            whole_hi = y;
            continue;
        }
        if (block_at(u) == g_blocks.end()) continue;
        sceKernelMprotect(reinterpret_cast<void*>(x), y - x, kRead | kWrite);
        memset(reinterpret_cast<void*>(x), 0, y - x);
        sceKernelMprotect(reinterpret_cast<void*>(x), y - x, kNone);
    }
    if (whole_hi > whole_lo) unback(whole_lo, whole_hi);
    return true;
}

void release(void* addr, size_t size) {
    uint64_t lo = reinterpret_cast<uint64_t>(addr), hi = lo + round_up(size, kUnit);
    {
        std::lock_guard lock(g_mu);
        for (auto it = g_blocks.lower_bound(lo); it != g_blocks.end() && it->first < hi;) {
            sceKernelMunmap(reinterpret_cast<void*>(it->first), it->second.len);
            sceKernelReleaseDirectMemory(it->second.phys, it->second.len);
            g_committed -= it->second.len;
            it = g_blocks.erase(it);
        }
    }
    sceKernelMunmap(addr, hi - lo);
}

bool protect(void* addr, size_t size, unsigned prot) { return sceKernelMprotect(addr, size, prot) == 0; }

size_t read_checked(void* dst, const void* src, size_t n) {
    size_t k = accessible(src, n, kRead);
    memcpy(dst, src, k);
    return k;
}

size_t write_checked(void* dst, const void* src, size_t n) {
    // Write-watched pages are read-only until written: count them as writable.
    size_t k = accessible(dst, n, kRead);
    uint64_t d = reinterpret_cast<uint64_t>(dst);
    write_watch_unprotect(d, d + k);
    k = accessible(dst, k, kWrite);
    memcpy(dst, src, k);
    return k;
}

// Direct memory we committed plus the flexible memory in use (libc's heap,
// stacks, the title's image).
uint64_t resident_bytes() {
    size_t configured = 0, available = 0;
    sceKernelConfiguredFlexibleMemorySize(&configured);
    sceKernelAvailableFlexibleMemorySize(&available);
    return g_committed.load() + (configured > available ? configured - available : 0);
}

std::string memory_report() {
    size_t blocks;
    {
        std::lock_guard lock(g_mu);
        blocks = g_blocks.size();
    }
    int64_t phys = 0;
    size_t largest = 0, configured = 0, available = 0;
    int64_t total = sceKernelGetDirectMemorySize();
    sceKernelAvailableDirectMemorySize(0, total, kUnit, &phys, &largest);
    sceKernelConfiguredFlexibleMemorySize(&configured);
    sceKernelAvailableFlexibleMemorySize(&available);
    char line[256];
    // The largest free block shrinks as RADV and DXVK take direct memory too.
    snprintf(line, sizeof line,
             "direct: ours %llu MiB in %zu blocks, largest free block %zu MiB of %lld; flexible %zu of %zu MiB used; "
             "address-less reservations at 0x%llx",
             (unsigned long long)(g_committed.load() >> 20), blocks, largest >> 20, (long long)(total >> 20),
             (configured - available) >> 20, configured >> 20, (unsigned long long)g_next_anywhere.load());
    return line;
}

}  // namespace fl::os
