// kernelx: VirtualAlloc/Free, process heap, XMem allocator.

#include <cstdio>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <map>
#include <mutex>
#include <sys/mman.h>

#include "common/log.h"
#include "hle/kernelx/kobject.h"
#include "hle/registry.h"

namespace fl::kx {
namespace {

constexpr DWORD MEM_COMMIT = 0x1000;
constexpr DWORD MEM_RESERVE = 0x2000;
constexpr DWORD MEM_DECOMMIT = 0x4000;
constexpr DWORD MEM_RELEASE = 0x8000;
constexpr DWORD MEM_TOP_DOWN = 0x100000;
constexpr DWORD MEM_LARGE_PAGES = 0x20000000;
constexpr DWORD kKnownTypeBits = MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN | MEM_LARGE_PAGES;

int to_prot(DWORD protect) {
    switch (protect & 0xFF) {
        case 0x01: return PROT_NONE;                          // PAGE_NOACCESS
        case 0x02: return PROT_READ;                          // PAGE_READONLY
        case 0x04: case 0x08: return PROT_READ | PROT_WRITE;  // READWRITE / WRITECOPY
        case 0x10: case 0x20: return PROT_READ | PROT_EXEC;
        case 0x40: case 0x80: return PROT_READ | PROT_WRITE | PROT_EXEC;
        default: return PROT_READ | PROT_WRITE;
    }
}

// Reservations: base -> size (needed for MEM_RELEASE, which passes size 0).
std::mutex g_vm_mutex;
std::map<uint64_t, uint64_t> g_regions;

WINAPI void* kx_VirtualAlloc(void* addr, SIZE_T size, DWORD type, DWORD protect) {
    if (type & ~kKnownTypeBits)
        FL_LOG(memory, "VirtualAlloc: unknown type bits 0x%x (Xbox-specific?)", type & ~kKnownTypeBits);
    if (protect & ~0xFFu) FL_LOG(memory, "VirtualAlloc: extra protect bits 0x%x (Xbox-specific?)", protect & ~0xFFu);
    size = (size + 0xFFF) & ~SIZE_T(0xFFF);
    int prot = (type & MEM_COMMIT) ? to_prot(protect) : PROT_NONE;
    std::lock_guard lock(g_vm_mutex);

    if (addr && !(type & MEM_RESERVE)) {
        // Commit inside an existing reservation.
        auto base = reinterpret_cast<uint64_t>(addr) & ~uint64_t(0xFFF);
        if (mprotect(reinterpret_cast<void*>(base), size, prot) != 0) {
            set_last_error(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
        return addr;
    }

    // Large allocations: align to 64 KB like Windows' allocation granularity.
    size_t align = 0x10000;
    void* want = addr;
    void* p = mmap(want, size + align, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | (want ? MAP_FIXED_NOREPLACE : 0),
                   -1, 0);
    if (p == MAP_FAILED) {
        FL_ERROR("VirtualAlloc(%p, 0x%llx, 0x%x, 0x%x) failed: %s", addr, (unsigned long long)size, type, protect,
                 strerror(errno));
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    auto raw = reinterpret_cast<uint64_t>(p);
    uint64_t aligned = want ? raw : (raw + align - 1) & ~(uint64_t(align) - 1);
    if (aligned > raw) munmap(p, aligned - raw);
    uint64_t end = raw + size + align;
    if (end > aligned + size) munmap(reinterpret_cast<void*>(aligned + size), end - (aligned + size));
    g_regions[aligned] = size;
    FL_LOG(memory, "VirtualAlloc 0x%llx bytes type 0x%x prot 0x%x -> 0x%llx", (unsigned long long)size, type, protect,
           (unsigned long long)aligned);
    return reinterpret_cast<void*>(aligned);
}

WINAPI BOOL kx_VirtualFree(void* addr, SIZE_T size, DWORD type) {
    std::lock_guard lock(g_vm_mutex);
    auto base = reinterpret_cast<uint64_t>(addr);
    if (type & MEM_RELEASE) {
        auto it = g_regions.find(base);
        if (it == g_regions.end()) return set_last_error(ERROR_INVALID_PARAMETER), FALSE_;
        munmap(addr, it->second);
        g_regions.erase(it);
        return TRUE_;
    }
    if (type & MEM_DECOMMIT) {
        size = (size + 0xFFF) & ~SIZE_T(0xFFF);
        mmap(addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
        return TRUE_;
    }
    return set_last_error(ERROR_INVALID_PARAMETER), FALSE_;
}

// ---------------------------------------------------------------- heap

HANDLE const kProcessHeap = reinterpret_cast<HANDLE>(0x7EAB0000);
constexpr DWORD HEAP_ZERO_MEMORY = 0x8;

WINAPI HANDLE kx_GetProcessHeap() { return kProcessHeap; }

WINAPI void* kx_HeapAlloc(HANDLE heap, DWORD flags, SIZE_T size) {
    void* p = (flags & HEAP_ZERO_MEMORY) ? calloc(1, size ? size : 1) : malloc(size ? size : 1);
    if (!p) set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return p;
}

WINAPI BOOL kx_HeapFree(HANDLE heap, DWORD flags, void* p) {
    free(p);
    return TRUE_;
}

// ---------------------------------------------------------------- XMem
// XMemAlloc(size, attributes) routes through hooks the title may install.
// The attribute bitfield is not decoded yet; we align generously.
// TODO(RE): decode XALLOC attributes (alignment, zero-init, memory type) from
// the game's call sites and record them in FINDINGS.md.

using XMemAllocFn = void*(WINAPI*)(SIZE_T size, uint64_t attributes);
using XMemFreeFn = void(WINAPI*)(void* p, uint64_t attributes);
XMemAllocFn g_alloc_hook = nullptr;
XMemFreeFn g_free_hook = nullptr;

WINAPI void* kx_XMemAllocDefault(SIZE_T size, uint64_t attributes) {
    size_t align = size >= 0x10000 ? 0x10000 : (size >= 0x1000 ? 0x1000 : 64);
    void* p = nullptr;
    if (posix_memalign(&p, align, size ? size : 1) != 0) return nullptr;
    memset(p, 0, size);
    FL_LOG(memory, "XMemAllocDefault(0x%llx, attr 0x%llx) -> %p", (unsigned long long)size,
           (unsigned long long)attributes, p);
    return p;
}

WINAPI void kx_XMemFreeDefault(void* p, uint64_t attributes) { free(p); }

WINAPI void* kx_XMemAlloc(SIZE_T size, uint64_t attributes) {
    return g_alloc_hook ? g_alloc_hook(size, attributes) : kx_XMemAllocDefault(size, attributes);
}

WINAPI void kx_XMemFree(void* p, uint64_t attributes) {
    if (g_free_hook)
        g_free_hook(p, attributes);
    else
        kx_XMemFreeDefault(p, attributes);
}

WINAPI BOOL kx_XMemSetAllocationHooks(XMemAllocFn alloc, XMemFreeFn freefn) {
    FL_LOG(memory, "XMemSetAllocationHooks(%p, %p)", reinterpret_cast<void*>(alloc), reinterpret_cast<void*>(freefn));
    g_alloc_hook = alloc;
    g_free_hook = freefn;
    return TRUE_;
}

// TITLEMEMORYSTATUS layout is not known yet: trap so the first caller tells us.
// TitleMemoryStatus (layout from kernelx.dll rva 0xb390): u32 length (must be
// 0x40), then seven u64: total, available (= total - used), then two
// used / peak-or-limit / available groups (names unknown; the second looks like
// the title's own budget). We report an Xbox One-sized 5 GiB title budget and
// our real resident memory as "used".
struct TitleMemoryStatusData {
    DWORD length;
    DWORD reserved;
    uint64_t total, available, used_a, peak_a, available_a, used_b, available_b;
};
static_assert(sizeof(TitleMemoryStatusData) == 0x40);

uint64_t resident_bytes() {
    FILE* f = fopen("/proc/self/statm", "r");
    unsigned long long pages = 0, rss = 0;
    if (f) {
        if (fscanf(f, "%llu %llu", &pages, &rss) != 2) rss = 0;
        fclose(f);
    }
    return rss * 4096;
}

WINAPI BOOL kx_TitleMemoryStatus(TitleMemoryStatusData* s) {
    if (s->length != sizeof(*s)) {
        set_last_error(87 /* ERROR_INVALID_PARAMETER */);
        return FALSE_;
    }
    constexpr uint64_t kTitleBudget = 5ull << 30;
    uint64_t used = std::min<uint64_t>(resident_bytes(), kTitleBudget - (256ull << 20));
    s->total = kTitleBudget;
    s->available = kTitleBudget - used;
    s->used_a = 0;
    s->peak_a = 0;
    s->available_a = 0;
    s->used_b = used;
    s->available_b = kTitleBudget - used;
    FL_STUB_ONCE("TitleMemoryStatus: 5 GiB budget, used = our resident memory");
    return TRUE_;
}

}  // namespace

HLE_EXPORT(kernelx, VirtualAlloc, kx_VirtualAlloc);
HLE_EXPORT(kernelx, VirtualFree, kx_VirtualFree);
HLE_EXPORT(kernelx, GetProcessHeap, kx_GetProcessHeap);
HLE_EXPORT(kernelx, HeapAlloc, kx_HeapAlloc);
HLE_EXPORT(kernelx, HeapFree, kx_HeapFree);
HLE_EXPORT(kernelx, XMemAlloc, kx_XMemAlloc);
HLE_EXPORT(kernelx, XMemFree, kx_XMemFree);
HLE_EXPORT(kernelx, XMemAllocDefault, kx_XMemAllocDefault);
HLE_EXPORT(kernelx, XMemFreeDefault, kx_XMemFreeDefault);
HLE_EXPORT(kernelx, XMemSetAllocationHooks, kx_XMemSetAllocationHooks);
HLE_EXPORT(kernelx, TitleMemoryStatus, kx_TitleMemoryStatus);

}  // namespace fl::kx
