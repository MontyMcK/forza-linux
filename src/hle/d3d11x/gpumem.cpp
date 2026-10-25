#include "hle/d3d11x/gpumem.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#include "common/log.h"

namespace fl::d3d::gpumem {
namespace {

constexpr uint64_t kPage = 4096;

int g_uffd = -1;     // userfaultfd, async write-protect mode
int g_pagemap = -1;  // /proc/self/pagemap, for PAGEMAP_SCAN
bool g_enabled = false;

void page_range(const void* addr, size_t size, uint64_t* start, uint64_t* end) {
    uint64_t a = reinterpret_cast<uint64_t>(addr);
    *start = a & ~(kPage - 1);
    *end = (a + (size ? size : 1) + kPage - 1) & ~(kPage - 1);
}

bool register_range(uint64_t start, uint64_t end) {
    uffdio_register reg{};
    reg.range.start = start;
    reg.range.len = end - start;
    reg.mode = UFFDIO_REGISTER_MODE_WP;
    return ioctl(g_uffd, UFFDIO_REGISTER, &reg) == 0;
}

// PAGEMAP_SCAN for written pages in [start, end). wp: also write-protect them
// (and fail with EPERM if part of the range is not registered). The walk stops
// once `out` is full; *walk_end says where.
long scan(uint64_t start, uint64_t end, bool wp, page_region* out, uint64_t n, uint64_t* walk_end = nullptr) {
    pm_scan_arg arg{};
    arg.size = sizeof arg;
    arg.flags = wp ? (PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC) : 0;
    arg.start = start;
    arg.end = end;
    arg.vec = reinterpret_cast<uint64_t>(out);
    arg.vec_len = n;
    arg.category_mask = PAGE_IS_WRITTEN;
    arg.return_mask = PAGE_IS_WRITTEN;
    long r = ioctl(g_pagemap, PAGEMAP_SCAN, &arg);
    if (walk_end) *walk_end = arg.walk_end;
    return r;
}

// Write-protect every written page of [start, end).
bool rearm(uint64_t start, uint64_t end) {
    page_region r[32];
    while (start < end) {
        uint64_t walk_end = end;
        long n = scan(start, end, true, r, 32, &walk_end);
        if (n < 0) return false;
        if (walk_end <= start) break;  // no progress: done
        start = walk_end;
    }
    return true;
}

}  // namespace

void init() {
    if (getenv("FL_NO_WRITE_TRACK")) {
        FL_STUB_ONCE("gpumem: FL_NO_WRITE_TRACK: every bind re-uploads placement memory");
        return;
    }
    // UFFD_USER_MODE_ONLY: what unprivileged processes may use
    // (vm.unprivileged_userfaultfd = 0). Async WP faults never reach
    // handle_userfault, so kernel-mode writes are recorded too.
    g_uffd = static_cast<int>(syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY));
    uffdio_api api{};
    api.api = UFFD_API;
    api.features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED;
    g_pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (g_uffd < 0 || ioctl(g_uffd, UFFDIO_API, &api) != 0 || g_pagemap < 0) {
        FL_ERROR("gpumem: userfaultfd async write-protect unavailable (%s): every bind re-uploads (slow)",
                 strerror(errno));
        return;
    }
    g_enabled = true;
}

void track(const void* addr, size_t size) {
    if (!g_enabled) return;
    uint64_t start, end;
    page_range(addr, size, &start, &end);
    // Registration does not protect anything: the pages read as written until
    // the first upload re-arms them. Re-registering a range is harmless.
    if (!register_range(start, end))
        FL_LOG(d3d, "gpumem: register %p+0x%llx failed (%s)", reinterpret_cast<void*>(start),
               (unsigned long long)(end - start), strerror(errno));
}

bool dirty(const void* addr, size_t size) {
    if (!g_enabled) return true;
    uint64_t start, end;
    page_range(addr, size, &start, &end);
    page_region r;
    long n = scan(start, end, false, &r, 1);
    return n != 0;  // written pages, or an error: upload to be safe
}

bool mapped(const void* addr, size_t size) {
    uint64_t start, end;
    page_range(addr, size, &start, &end);
    return msync(reinterpret_cast<void*>(start), end - start, MS_ASYNC) == 0;
}

bool readable(const void* addr, size_t size) {
    // One byte from every page, up to 1024 pages per call: process_vm_readv
    // stops at the first page it cannot read.
    uint64_t start, end;
    page_range(addr, size, &start, &end);
    constexpr size_t kBatch = 1024;
    iovec remote[kBatch];
    uint8_t sink[kBatch];
    while (start < end) {
        size_t n = 0;
        for (; n < kBatch && start < end; n++, start += kPage) remote[n] = {reinterpret_cast<void*>(start), 1};
        iovec local{sink, n};
        if (process_vm_readv(getpid(), &local, 1, remote, n, 0) != static_cast<ssize_t>(n)) return false;
    }
    return true;
}

void begin_upload(const void* addr, size_t size) {
    if (!g_enabled) return;
    static const bool no_wp = getenv("FL_UFFD_NO_WP") != nullptr;  // bisect aid
    if (no_wp) return;
    uint64_t start, end;
    page_range(addr, size, &start, &end);
    if (rearm(start, end)) return;
    // Part of the range lost its registration: the game decommitted or
    // remapped it (a new mapping). Register it again and re-arm.
    if (errno == EPERM && register_range(start, end) && rearm(start, end)) return;
    FL_LOG(d3d, "gpumem: re-arming %p+0x%llx failed (%s); it stays dirty", reinterpret_cast<void*>(start),
           (unsigned long long)(end - start), strerror(errno));
}

}  // namespace fl::d3d::gpumem
