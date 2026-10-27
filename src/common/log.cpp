#include "common/log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

namespace fl {
namespace {

std::string g_channels;
bool g_all = false;
std::mutex g_mutex;
FILE* g_file = nullptr;
timespec g_start;

double elapsed() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - g_start.tv_sec) + (now.tv_nsec - g_start.tv_nsec) / 1e9;
}

void vwrite(const char* channel, const char* fmt, va_list ap) {
    char msg[2048];
    vsnprintf(msg, sizeof msg, fmt, ap);
    std::lock_guard lock(g_mutex);
    long tid = syscall(SYS_gettid);
    fprintf(stderr, "[%9.4f %6ld %-8s] %s\n", elapsed(), tid, channel, msg);
    if (g_file) {
        fprintf(g_file, "[%9.4f %6ld %-8s] %s\n", elapsed(), tid, channel, msg);
        fflush(g_file);
    }
}

}  // namespace

double run_time() { return elapsed(); }

void log_init() {
    clock_gettime(CLOCK_MONOTONIC, &g_start);
    if (const char* c = getenv("FL_LOG")) {
        g_channels = std::string(",") + c + ",";
        g_all = g_channels.find(",all,") != std::string::npos;
    }
    if (const char* f = getenv("FL_LOG_FILE")) g_file = fopen(f, "w");
}

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
