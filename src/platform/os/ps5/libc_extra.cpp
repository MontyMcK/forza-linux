// libc functions the runtime uses that a PS5 title doesn't get from the
// console (the link leaves them undefined). Our own implementations.
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <sys/types.h>

extern "C" {

// timegm: struct tm in UTC to time_t (days from the civil date, as in
// Howard Hinnant's public-domain algorithm).
time_t timegm(struct tm* t) {
    long long y = t->tm_year + 1900LL, m = t->tm_mon;
    y += m / 12;  // months outside 0..11 roll into the year
    m %= 12;
    if (m < 0) {
        m += 12;
        y--;
    }
    m += 1;  // 1..12
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + t->tm_mday - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + doe - 719468;
    return static_cast<time_t>(days * 86400 + t->tm_hour * 3600LL + t->tm_min * 60LL + t->tm_sec);
}

const char* strsignal(int sig) {
    static thread_local char buf[32];
    snprintf(buf, sizeof buf, "signal %d", sig);
    return buf;
}

// Only libkernel_sys exports these, which a title doesn't get (the import would
// be NULL). The runtime copies when link() fails (ConnectedStorage); libc++'s
// std::filesystem references the rest. Titles have no hard or symbolic links.
int link(const char*, const char*) {
    errno = EPERM;
    return -1;
}
int symlink(const char*, const char*) {
    errno = EPERM;
    return -1;
}
ssize_t readlink(const char*, char*, size_t) {
    errno = EINVAL;  // "not a symbolic link"
    return -1;
}
long pathconf(const char*, int) {
    errno = EINVAL;  // callers fall back to PATH_MAX
    return -1;
}

}  // extern "C"
