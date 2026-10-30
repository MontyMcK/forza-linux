// Diagnostics for crash, freeze and CPU-profile reports.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fl::os {

struct ThreadInfo {
    uint64_t tid;         // a thread_id()
    std::string name;
    double cpu_seconds;   // user + system time so far
};

// Every thread of this process (empty if the host can't list them).
std::vector<ThreadInfo> list_threads();

// A thread's name ("?" if unknown).
std::string thread_name(uint64_t tid);

// The host's description of the mapping that holds `addr`; "" if `addr` is
// not mapped, "(unknown)" if the host can't say.
std::string mapping_of(uint64_t addr);

}  // namespace fl::os
