// Write watching with userfaultfd write-protection in async mode: the kernel
// records the first write to each protected page itself (no fault reaches us,
// and kernel writes such as read() into the page work), and PAGEMAP_SCAN
// reports written pages and re-protects them. Replaced an mprotect + SIGSEGV
// scheme that broke kernel writes and raced between threads (2026-10-25).
#include "platform/os/write_watch.h"

#include <cerrno>
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
#include "platform/os/memory.h"

namespace fl::os {
namespace {

int g_uffd = -1;     // userfaultfd, async write-protect mode
int g_pagemap = -1;  // /proc/self/pagemap, for PAGEMAP_SCAN

// PAGEMAP_SCAN for written pages in [start, end). wp: also write-protect them
// (and fail with EPERM if part of the range is not registered). The walk stops
// once `out` is full; *walk_end says where.
// strict = false: memory without async write-protect (not tracked) is skipped
// instead of failing the scan (the kernel's pagemap_scan_test_walk).
long scan(uint64_t start, uint64_t end, bool wp, page_region* out, uint64_t n, uint64_t* walk_end = nullptr,
          bool strict = true) {
    pm_scan_arg arg{};
    arg.size = sizeof arg;
    arg.flags = wp ? (PM_SCAN_WP_MATCHING | (strict ? PM_SCAN_CHECK_WPASYNC : 0)) : 0;
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

}  // namespace

bool write_watch_init() {
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
        return false;
    }
    return true;
}

bool write_watch_register(uint64_t start, uint64_t end) {
    uffdio_register reg{};
    reg.range.start = start;
    reg.range.len = end - start;
    reg.mode = UFFDIO_REGISTER_MODE_WP;
    return ioctl(g_uffd, UFFDIO_REGISTER, &reg) == 0;
}

bool write_watch_reset(uint64_t start, uint64_t end, bool strict, WrittenFn on_written, void* user, size_t* calls) {
    // Strict resets cover one resource; the non-strict frame scan walks
    // everything tracked, about one ioctl per 256 written regions.
    page_region r[256];
    const uint64_t batch = strict ? 32 : 256;
    size_t n_calls = 0;
    bool ok = true;
    while (start < end) {
        uint64_t walk_end = end;
        long n = scan(start, end, true, r, batch, &walk_end, strict);
        n_calls++;
        if (n < 0) {
            ok = false;
            break;
        }
        if (on_written)
            for (long i = 0; i < n; i++) on_written(r[i].start, r[i].end, user);
        if (walk_end <= start) break;  // no progress: done
        start = walk_end;
    }
    if (calls) *calls = n_calls;
    return ok;
}

bool write_watch_written(uint64_t start, uint64_t end) {
    page_region r;
    long n = scan(start, end, false, &r, 1);
    return n != 0;  // written pages, or an error: upload to be safe
}

bool pages_mapped(uint64_t start, uint64_t end) {
    return msync(reinterpret_cast<void*>(start), end - start, MS_ASYNC) == 0;
}

bool pages_readable(uint64_t start, uint64_t end) {
    // One byte from every page, up to 1024 pages per call: process_vm_readv
    // stops at the first page it cannot read.
    const uint64_t page = page_size();
    constexpr size_t kBatch = 1024;
    iovec remote[kBatch];
    uint8_t sink[kBatch];
    while (start < end) {
        size_t n = 0;
        for (; n < kBatch && start < end; n++, start += page) remote[n] = {reinterpret_cast<void*>(start), 1};
        iovec local{sink, n};
        if (process_vm_readv(getpid(), &local, 1, remote, n, 0) != static_cast<ssize_t>(n)) return false;
    }
    return true;
}

}  // namespace fl::os
