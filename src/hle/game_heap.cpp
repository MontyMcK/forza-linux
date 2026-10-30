#include "hle/game_heap.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "platform/os/memory.h"

extern "C" size_t malloc_usable_size(void* p);  // glibc; the PS5 platform heap has it too

namespace fl::heap {
namespace {

constexpr size_t kLarge = 0x80000;
std::mutex g_mu;
std::unordered_map<void*, size_t> g_large;  // block -> mapped size

// Large blocks are page aligned; libc's blocks almost never are, so only
// page-aligned pointers need the lookup.
bool large_size(void* p, size_t* n) {
    if (reinterpret_cast<uintptr_t>(p) & (os::page_size() - 1)) return false;
    std::lock_guard lock(g_mu);
    auto it = g_large.find(p);
    if (it == g_large.end()) return false;
    *n = it->second;
    return true;
}

}  // namespace

void* alloc(size_t n, bool zero) {
    if (n < kLarge) return zero ? calloc(1, n ? n : 1) : malloc(n ? n : 1);
    void* p = os::map_anon(nullptr, n, os::kRead | os::kWrite);  // zero-filled
    if (p) {
        std::lock_guard lock(g_mu);
        g_large[p] = n;
    }
    return p;
}

void free(void* p) {
    if (!p) return;
    size_t n;
    if (large_size(p, &n)) {
        {
            std::lock_guard lock(g_mu);
            g_large.erase(p);
        }
        os::release(p, n);
        return;
    }
    ::free(p);
}

void* realloc(void* p, size_t n) {
    if (!p) return alloc(n, false);
    if (n == 0) {
        free(p);
        return nullptr;
    }
    size_t old;
    bool large = large_size(p, &old);
    if (!large && n < kLarge) return ::realloc(p, n);
    if (!large) old = malloc_usable_size(p);
    void* q = alloc(n, false);
    if (!q) return nullptr;
    memcpy(q, p, std::min(old, n));
    free(p);
    return q;
}

}  // namespace fl::heap
