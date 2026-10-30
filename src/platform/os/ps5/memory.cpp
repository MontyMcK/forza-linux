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
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), len, kUnit, kDirectTypeCpu, &phys) != 0) {
        errno = ENOMEM;
        return false;
    }
    void* va = reinterpret_cast<void*>(rs);
    if (sceKernelMapDirectMemory(&va, len, kRead | kWrite, kMapFixed, phys, kUnit) != 0 || va != reinterpret_cast<void*>(rs)) {
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
    for (uint64_t hint = round_up(g_next_anywhere.load(), align); hint + size <= kAnywhereHi;) {
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
    return nullptr;
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
    for (uint64_t u = ulo; u < uhi;) {
        if (block_at(u) != g_blocks.end()) {
            u += kUnit;
            continue;
        }
        uint64_t re = u;
        while (re < uhi && block_at(re) == g_blocks.end()) re += kUnit;
        if (!back_run(u, re)) {
            int64_t phys = 0;
            size_t largest = 0;
            sceKernelAvailableDirectMemorySize(0, sceKernelGetDirectMemorySize(), kUnit, &phys, &largest);
            FL_ERROR("os: commit %p+0x%llx: no direct memory (we hold %llu MiB; largest free block %zu MiB of %lld MiB)",
                     reinterpret_cast<void*>(u), (unsigned long long)(re - u), (unsigned long long)(g_committed.load() >> 20),
                     largest >> 20, (long long)(sceKernelGetDirectMemorySize() >> 20));
            return false;
        }
        // Pages of the new units outside the request stay inaccessible (reserved).
        if (u < lo) sceKernelMprotect(reinterpret_cast<void*>(u), lo - u, kNone);
        if (re > hi) sceKernelMprotect(reinterpret_cast<void*>(hi), re - hi, kNone);
        u = re;
    }
    return sceKernelMprotect(reinterpret_cast<void*>(lo), hi - lo, prot) == 0;
}

bool decommit(void* addr, size_t size) {
    // The direct memory stays allocated until release() (it is handed out in
    // 64 KiB units): the pages are zeroed and made inaccessible.
    uint64_t lo = reinterpret_cast<uint64_t>(addr) & ~(kPage - 1);
    uint64_t hi = round_up(reinterpret_cast<uint64_t>(addr) + size, kPage);
    std::lock_guard lock(g_mu);
    for (uint64_t p = lo; p < hi; p += kPage) {
        if (block_at(p & ~(kUnit - 1)) == g_blocks.end()) continue;
        sceKernelMprotect(reinterpret_cast<void*>(p), kPage, kRead | kWrite);
        memset(reinterpret_cast<void*>(p), 0, kPage);
        sceKernelMprotect(reinterpret_cast<void*>(p), kPage, kNone);
    }
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

}  // namespace fl::os
