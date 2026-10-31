#include "hle/d3d11x/gpumem.h"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

#include "common/log.h"
#include "platform/os/memory.h"
#include "platform/os/write_watch.h"

namespace fl::d3d::gpumem {
namespace {

bool g_enabled = false;

// Every tracked range, merged where ranges touch (start -> end).
std::mutex g_ranges_mutex;
std::map<uint64_t, uint64_t> g_ranges;

void add_range(uint64_t start, uint64_t end) {
    std::lock_guard lock(g_ranges_mutex);
    auto it = g_ranges.upper_bound(start);
    if (it != g_ranges.begin() && std::prev(it)->second >= start) {
        --it;
        start = it->first;
        end = std::max(end, it->second);
        it = g_ranges.erase(it);
    }
    while (it != g_ranges.end() && it->first <= end) {
        end = std::max(end, it->second);
        it = g_ranges.erase(it);
    }
    g_ranges[start] = end;
}

void page_range(const void* addr, size_t size, uint64_t* start, uint64_t* end) {
    const uint64_t page = os::page_size();
    uint64_t a = reinterpret_cast<uint64_t>(addr);
    *start = a & ~(page - 1);
    *end = (a + (size ? size : 1) + page - 1) & ~(page - 1);
}

}  // namespace

void init() {
    if (getenv("FL_NO_WRITE_TRACK")) {
        FL_STUB_ONCE("gpumem: FL_NO_WRITE_TRACK: every bind re-uploads placement memory");
        return;
    }
    g_enabled = os::write_watch_init();
}

void track(const void* addr, size_t size) {
    if (!g_enabled) return;
    uint64_t start, end;
    page_range(addr, size, &start, &end);
    // Registration does not protect anything: the pages read as written until
    // the first upload re-arms them. Re-registering a range is harmless.
    if (!os::write_watch_register(start, end))
        FL_LOG(d3d, "gpumem: register %p+0x%llx failed (%s)", reinterpret_cast<void*>(start),
               (unsigned long long)(end - start), strerror(errno));
    else
        add_range(start, end);
}

size_t rearm_all(WrittenFn on_written, void* user, uint32_t part, uint32_t parts) {
    if (!g_enabled || !parts) return 0;
    const uint64_t page = os::page_size();
    uint64_t lo, hi;
    {
        std::lock_guard lock(g_ranges_mutex);
        if (g_ranges.empty()) return 0;
        lo = g_ranges.begin()->first;
        hi = g_ranges.rbegin()->second;
    }
    uint64_t span = hi - lo;
    uint64_t a = lo + (span / parts * part & ~(page - 1));
    uint64_t b = part + 1 == parts ? hi : lo + (span / parts * (part + 1) & ~(page - 1));
    // One walk over everything tracked: untracked memory and holes in between
    // are skipped (non-strict reset).
    size_t calls = 0;
    if (!os::write_watch_reset(a, b, false, on_written, user, &calls))
        FL_LOG(d3d, "gpumem: frame scan %p-%p failed (%s)", reinterpret_cast<void*>(a), reinterpret_cast<void*>(b),
               strerror(errno));
    return calls;
}

bool dirty(const void* addr, size_t size) {
    if (!g_enabled) return true;
    uint64_t start, end;
    page_range(addr, size, &start, &end);
    return os::write_watch_written(start, end);
}

bool mapped(const void* addr, size_t size) {
    uint64_t start, end;
    page_range(addr, size, &start, &end);
    return os::pages_mapped(start, end);
}

bool readable(const void* addr, size_t size) {
    uint64_t start, end;
    page_range(addr, size, &start, &end);
    return os::pages_readable(start, end);
}

bool begin_upload(const void* addr, size_t size, WrittenFn on_written, void* user) {
    if (!g_enabled) return false;
    static const bool no_wp = getenv("FL_UFFD_NO_WP") != nullptr;  // bisect aid
    if (no_wp) return false;
    uint64_t start, end;
    page_range(addr, size, &start, &end);
    if (os::write_watch_reset(start, end, true, on_written, user)) return true;
    // Part of the range lost its registration: the game decommitted or
    // remapped it (a new mapping). Register it again and re-arm.
    if (errno == EPERM && os::write_watch_register(start, end) && os::write_watch_reset(start, end, true, on_written, user)) {
        add_range(start, end);
        return true;
    }
    FL_LOG(d3d, "gpumem: re-arming %p+0x%llx failed (%s); it stays dirty", reinterpret_cast<void*>(start),
           (unsigned long long)(end - start), strerror(errno));
    return false;
}

namespace {
std::atomic<RemapFn> g_remap_hook{nullptr};
std::shared_mutex g_unmap_lock;
}

std::shared_lock<std::shared_mutex> read_guard() { return std::shared_lock(g_unmap_lock); }
std::unique_lock<std::shared_mutex> unmap_guard() { return std::unique_lock(g_unmap_lock); }

void set_remap_hook(RemapFn fn) { g_remap_hook = fn; }

void notify_remap(const void* addr, size_t size) {
    uint64_t start, end;
    page_range(addr, size, &start, &end);
    os::write_watch_unregister(start, end);
    if (RemapFn fn = g_remap_hook.load()) fn(start, end);
}

}  // namespace fl::d3d::gpumem
