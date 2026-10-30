#include "platform/os/debug.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <unistd.h>

namespace fl::os {

std::string thread_name(uint64_t tid) {
    char path[64], name[64] = "?";
    snprintf(path, sizeof path, "/proc/self/task/%llu/comm", (unsigned long long)tid);
    if (FILE* f = fopen(path, "r")) {
        if (fgets(name, sizeof name, f)) name[strcspn(name, "\n")] = 0;
        fclose(f);
    }
    return name;
}

std::vector<ThreadInfo> list_threads() {
    std::vector<ThreadInfo> out;
    DIR* dir = opendir("/proc/self/task");
    if (!dir) return out;
    while (dirent* e = readdir(dir)) {
        if (e->d_name[0] == '.') continue;
        uint64_t tid = strtoull(e->d_name, nullptr, 10);
        // CPU time so far (utime + stime, fields 14-15 of stat): two dumps a
        // few seconds apart show which threads burn CPU.
        double cpu = 0;
        if (FILE* f = fopen(("/proc/self/task/" + std::string(e->d_name) + "/stat").c_str(), "r")) {
            char line[1024];
            if (fgets(line, sizeof line, f))
                if (const char* p = strrchr(line, ')')) {
                    unsigned long long ut = 0, st = 0;
                    if (sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) == 2)
                        cpu = double(ut + st) / sysconf(_SC_CLK_TCK);
                }
            fclose(f);
        }
        out.push_back({tid, thread_name(tid), cpu});
    }
    closedir(dir);
    return out;
}

std::string mapping_of(uint64_t addr) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return "(unknown)";
    char line[256];
    std::string found;
    while (fgets(line, sizeof line, f)) {
        unsigned long long lo, hi;
        if (sscanf(line, "%llx-%llx", &lo, &hi) == 2 && addr >= lo && addr < hi) {
            line[strcspn(line, "\n")] = 0;
            found = line;
            break;
        }
    }
    fclose(f);
    return found;
}

}  // namespace fl::os
