// libc functions the runtime uses that a PS5 title doesn't get from the
// console (the link leaves them undefined). Our own implementations.
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
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

char* strsignal(int sig) {
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

// mkstemp: also libkernel_sys-only (DXVK's and Mesa's cache files use it).
int mkstemp(char* tmpl) {
    size_t n = strlen(tmpl);
    if (n < 6 || strcmp(tmpl + n - 6, "XXXXXX") != 0) {
        errno = EINVAL;
        return -1;
    }
    static const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (int attempt = 0; attempt < 100; attempt++) {
        uint32_t r[6];
        arc4random_buf(r, sizeof r);
        for (int i = 0; i < 6; i++) tmpl[n - 6 + i] = chars[r[i] % (sizeof chars - 1)];
        int fd = open(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 || errno != EEXIST) return fd;
    }
    errno = EEXIST;
    return -1;
}

}  // extern "C"
