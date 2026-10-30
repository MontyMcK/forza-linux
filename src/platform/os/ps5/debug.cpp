#include "platform/os/debug.h"

#include <cstdio>
#include <map>
#include <pthread.h>

#include "platform/os/ps5/kernel.h"

namespace fl::os {

std::map<uint64_t, pthread_t> known_threads();  // thread.cpp

// A title can't list its threads (no /proc): the ones that asked for their
// id (every game thread does) are what we know. Names and CPU times unknown.
std::vector<ThreadInfo> list_threads() {
    std::vector<ThreadInfo> out;
    for (auto& [tid, t] : known_threads()) out.push_back({tid, "?", 0});
    return out;
}

std::string thread_name(uint64_t) { return "?"; }

std::string mapping_of(uint64_t addr) {
    void *start = nullptr, *end = nullptr;
    uint32_t prot = 0;
    if (sceKernelQueryMemoryProtection(reinterpret_cast<void*>(addr), &start, &end, &prot) != 0) return "";
    char line[96];
    snprintf(line, sizeof line, "%p-%p prot 0x%x", start, end, prot);
    return line;
}

}  // namespace fl::os
