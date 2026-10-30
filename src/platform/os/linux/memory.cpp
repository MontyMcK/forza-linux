#include "platform/os/memory.h"

#include <cstdio>
#include <sys/mman.h>
#include <unistd.h>

namespace fl::os {
namespace {

int to_prot(unsigned p) {
    return (p & kRead ? PROT_READ : 0) | (p & kWrite ? PROT_WRITE : 0) | (p & kExec ? PROT_EXEC : 0);
}

size_t round_up(size_t n, size_t a) { return (n + a - 1) & ~(a - 1); }

}  // namespace

size_t page_size() {
    static const size_t size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    return size;
}

void* map_anon(void* want, size_t size, unsigned prot, size_t align) {
    const size_t page = page_size();
    size = round_up(size ? size : 1, page);
    constexpr int kFlags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
    if (want) {
        void* p = mmap(want, size, to_prot(prot), kFlags | MAP_FIXED_NOREPLACE, -1, 0);
        return p == MAP_FAILED ? nullptr : p;
    }
    if (align <= page) {
        void* p = mmap(nullptr, size, to_prot(prot), kFlags, -1, 0);
        return p == MAP_FAILED ? nullptr : p;
    }
    // Over-map and trim to the alignment.
    void* p = mmap(nullptr, size + align, to_prot(prot), kFlags, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    auto raw = reinterpret_cast<uintptr_t>(p);
    uintptr_t base = round_up(raw, align);
    if (base > raw) munmap(p, base - raw);
    uintptr_t end = raw + size + align;
    if (end > base + size) munmap(reinterpret_cast<void*>(base + size), end - (base + size));
    return reinterpret_cast<void*>(base);
}

bool commit(void* addr, size_t size, unsigned prot) {
    // The range is a lazily backed anonymous mapping: making it accessible
    // keeps pages already touched and shows zeros for the rest.
    return mprotect(addr, size, to_prot(prot)) == 0;
}

bool decommit(void* addr, size_t size) {
    return mmap(addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0) != MAP_FAILED;
}

void release(void* addr, size_t size) { munmap(addr, size); }

bool protect(void* addr, size_t size, unsigned prot) { return mprotect(addr, size, to_prot(prot)) == 0; }

uint64_t resident_bytes() {
    FILE* f = fopen("/proc/self/statm", "r");
    unsigned long long pages = 0, rss = 0;
    if (f) {
        if (fscanf(f, "%llu %llu", &pages, &rss) != 2) rss = 0;
        fclose(f);
    }
    return rss * page_size();
}

}  // namespace fl::os
