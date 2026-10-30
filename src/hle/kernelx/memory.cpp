// kernelx: VirtualAlloc/Free, process heap, XMem allocator.

#include <cstdio>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

#include "common/log.h"
#include "hle/d3d11x/gpumem.h"
#include "hle/kernelx/kobject.h"
#include "hle/registry.h"
#include "platform/os/memory.h"

namespace fl::kx {
namespace {

constexpr DWORD MEM_COMMIT = 0x1000;
constexpr DWORD MEM_RESERVE = 0x2000;
constexpr DWORD MEM_DECOMMIT = 0x4000;
constexpr DWORD MEM_RELEASE = 0x8000;
constexpr DWORD MEM_TOP_DOWN = 0x100000;
constexpr DWORD MEM_LARGE_PAGES = 0x20000000;
constexpr DWORD kKnownTypeBits = MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN | MEM_LARGE_PAGES;

unsigned to_prot(DWORD protect) {
    switch (protect & 0xFF) {
        case 0x01: return os::kNone;                          // PAGE_NOACCESS
        case 0x02: return os::kRead;                          // PAGE_READONLY
        case 0x04: case 0x08: return os::kRead | os::kWrite;  // READWRITE / WRITECOPY
        case 0x10: case 0x20: return os::kRead | os::kExec;
        case 0x40: case 0x80: return os::kRead | os::kWrite | os::kExec;
        default: return os::kRead | os::kWrite;
    }
}

SIZE_T page_round(SIZE_T n) { return (n + os::page_size() - 1) & ~SIZE_T(os::page_size() - 1); }

// Reservations: base -> size (needed for MEM_RELEASE, which passes size 0).
std::mutex g_vm_mutex;
std::map<uint64_t, uint64_t> g_regions;

WINAPI void* kx_VirtualAlloc(void* addr, SIZE_T size, DWORD type, DWORD protect) {
    if (type & ~kKnownTypeBits)
        FL_LOG(memory, "VirtualAlloc: unknown type bits 0x%x (Xbox-specific?)", type & ~kKnownTypeBits);
    if (protect & ~0xFFu) FL_LOG(memory, "VirtualAlloc: extra protect bits 0x%x (Xbox-specific?)", protect & ~0xFFu);
    size = page_round(size);
    unsigned prot = (type & MEM_COMMIT) ? to_prot(protect) : os::kNone;
    std::lock_guard lock(g_vm_mutex);

    if (addr && !(type & MEM_RESERVE)) {
        // Commit inside an existing reservation.
        auto base = reinterpret_cast<uint64_t>(addr) & ~uint64_t(os::page_size() - 1);
        FL_LOG(memory, "VirtualAlloc commit %p+0x%llx type 0x%x prot 0x%x", addr, (unsigned long long)size, type, protect);
        if (!os::commit(reinterpret_cast<void*>(base), size, prot)) {
            set_last_error(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
        return addr;
    }

    // Align to 64 KB like Windows' allocation granularity.
    void* p = os::map_anon(addr, size, prot, 0x10000);
    if (!p) {
        FL_ERROR("VirtualAlloc(%p, 0x%llx, 0x%x, 0x%x) failed: %s", addr, (unsigned long long)size, type, protect,
                 strerror(errno));
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    auto aligned = reinterpret_cast<uint64_t>(p);
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
        FL_LOG(memory, "VirtualFree release %p+0x%llx", addr, (unsigned long long)it->second);
        auto guard = d3d::gpumem::unmap_guard();  // not while an upload reads it
        os::release(addr, it->second);
        d3d::gpumem::notify_remap(addr, it->second);
        g_regions.erase(it);
        return TRUE_;
    }
    if (type & MEM_DECOMMIT) {
        size = page_round(size);
        FL_LOG(memory, "VirtualFree decommit %p+0x%llx", addr, (unsigned long long)size);
        auto guard = d3d::gpumem::unmap_guard();  // not while an upload reads it
        os::decommit(addr, size);
        d3d::gpumem::notify_remap(addr, size);  // the fresh pages lost their write tracking
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
    if (size >= 0x10000) FL_LOG(memory, "HeapAlloc(0x%llx) -> %p", (unsigned long long)size, p);
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

// Title memory gets its own page-rounded mappings, never the C heap: the game
// places GPU resources in it (CreatePlacement*), and the D3D layer write-protects
// those pages to see CPU writes (d3d11x/gpumem.h). On shared heap pages that
// protection also covered malloc metadata and DXVK/RADV objects, and kernel
// writes into them (ioctl results) failed with EFAULT: DXVK then crashed at
// random (2026-10-25). Azimuth likewise keeps title memory in its own mappings.
std::mutex g_xmem_mutex;
std::map<uintptr_t, size_t> g_xmem;  // base -> mapped length

WINAPI void* kx_XMemAllocDefault(SIZE_T size, uint64_t attributes) {
    size_t len = page_round(size ? size : 1);
    void* p = os::map_anon(nullptr, len, os::kRead | os::kWrite, size >= 0x10000 ? 0x10000 : 0);
    if (!p) {
        FL_ERROR("XMemAllocDefault(0x%llx): mmap failed", (unsigned long long)size);
        return nullptr;
    }
    auto base = reinterpret_cast<uintptr_t>(p);
    {
        std::lock_guard lock(g_xmem_mutex);
        g_xmem[base] = len;
    }
    FL_LOG(memory, "XMemAllocDefault(0x%llx, attr 0x%llx) -> %p", (unsigned long long)size,
           (unsigned long long)attributes, reinterpret_cast<void*>(base));
    return reinterpret_cast<void*>(base);  // map_anon memory is already zero
}

WINAPI void kx_XMemFreeDefault(void* p, uint64_t attributes) {
    if (!p) return;
    size_t len = 0;
    {
        std::lock_guard lock(g_xmem_mutex);
        auto it = g_xmem.find(reinterpret_cast<uintptr_t>(p));
        if (it == g_xmem.end()) {
            FL_ERROR("XMemFreeDefault(%p): not a block XMemAllocDefault returned", p);
            return;
        }
        len = it->second;
        g_xmem.erase(it);
    }
    auto guard = d3d::gpumem::unmap_guard();  // not while an upload reads it
    os::release(p, len);
}

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

WINAPI BOOL kx_TitleMemoryStatus(TitleMemoryStatusData* s) {
    if (s->length != sizeof(*s)) {
        set_last_error(87 /* ERROR_INVALID_PARAMETER */);
        return FALSE_;
    }
    constexpr uint64_t kTitleBudget = 5ull << 30;
    uint64_t used = std::min<uint64_t>(os::resident_bytes(), kTitleBudget - (256ull << 20));
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
