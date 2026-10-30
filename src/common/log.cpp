#include "common/log.h"
#include "platform/os/thread.h"

#include <fcntl.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/syscall.h>
#include <thread>
#include <time.h>
#include <unistd.h>

namespace fl {
namespace {

std::string g_channels;
bool g_all = false;
std::mutex g_mutex;        // stderr and g_pending
std::mutex g_write_mutex;  // writes to the log file, kept in order
int g_fd = -1;
std::string g_pending;     // lines not in the file yet
timespec g_start;

double elapsed() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - g_start.tv_sec) + (now.tv_nsec - g_start.tv_nsec) / 1e9;
}

// The log file gets lines in batches (every 200 ms, and at once after an
// ERROR or FATAL line): flushing each line under g_mutex made a busy channel
// (~1500 d3d lines a second) stall every thread on the PS5, where a write to
// /data is slow (FINDINGS 7.12).
void write_pending() {
    if (g_fd < 0) return;
    std::lock_guard w(g_write_mutex);
    std::string out;
    {
        std::lock_guard lock(g_mutex);
        out.swap(g_pending);
    }
    for (size_t done = 0; done < out.size();) {
        ssize_t n = write(g_fd, out.data() + done, out.size() - done);
        if (n <= 0) break;
        done += size_t(n);
    }
}

void vwrite(const char* channel, const char* fmt, va_list ap) {
    char msg[2048];
    vsnprintf(msg, sizeof msg, fmt, ap);
    char line[2200];
    snprintf(line, sizeof line, "[%9.4f %6ld %-8s] %s\n", elapsed(), static_cast<long>(os::thread_id()), channel, msg);
    {
        std::lock_guard lock(g_mutex);
        fputs(line, stderr);
        if (g_fd >= 0) g_pending += line;
    }
    if (!strcmp(channel, "ERROR") || !strcmp(channel, "FATAL")) write_pending();
}

}  // namespace

double run_time() { return elapsed(); }

void log_init() {
    clock_gettime(CLOCK_MONOTONIC, &g_start);
    if (const char* c = getenv("FL_LOG")) {
        g_channels = std::string(",") + c + ",";
        g_all = g_channels.find(",all,") != std::string::npos;
    }
    if (const char* f = getenv("FL_LOG_FILE")) g_fd = open(f, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (g_fd >= 0)
        std::thread([] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                write_pending();
            }
        }).detach();
}

void log_flush() { write_pending(); }

bool log_enabled(const char* channel) {
    if (g_all) return true;
    if (g_channels.empty()) return false;
    char key[64];
    snprintf(key, sizeof key, ",%s,", channel);
    return g_channels.find(key) != std::string::npos;
}

void log_write(const char* channel, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vwrite(channel, fmt, ap);
    va_end(ap);
}

void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vwrite("FATAL", fmt, ap);
    va_end(ap);
    fflush(stderr);
    abort();
}

}  // namespace fl
