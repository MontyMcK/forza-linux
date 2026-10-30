// Logging. Channels are enabled with FL_LOG=chan1,chan2 or FL_LOG=all.
// Errors (FL_ERROR) and UNIMPLEMENTED are always printed.
#pragma once

#include <cstdarg>

namespace fl {

void log_init();
bool log_enabled(const char* channel);
// Seconds since log_init (the timestamps in log lines).
double run_time();
void log_write(const char* channel, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
// Writes buffered lines to FL_LOG_FILE now (exit paths; ERROR/FATAL lines flush by themselves).
void log_flush();
[[noreturn]] void fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace fl

#define FL_LOG(chan, ...)                                   \
    do {                                                    \
        if (::fl::log_enabled(#chan)) ::fl::log_write(#chan, __VA_ARGS__); \
    } while (0)

#define FL_ERROR(...) ::fl::log_write("ERROR", __VA_ARGS__)

// Log once per call site that a fake-success stub was hit.
#define FL_STUB_ONCE(what)                                  \
    do {                                                    \
        static bool fl_stub_seen_ = false;                  \
        if (!fl_stub_seen_) {                               \
            fl_stub_seen_ = true;                           \
            ::fl::log_write("STUB", "%s", what);            \
        }                                                   \
    } while (0)
