#include "platform/os/files.h"

#include <sys/stat.h>

namespace fl::os {

// stat, never access(): access() fails with EPERM for every path in a title.
bool path_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

// Of a title's 250 descriptors: the rest are for DXVK/RADV, saves, logs, sockets.
int open_file_budget() { return 128; }

}  // namespace fl::os
