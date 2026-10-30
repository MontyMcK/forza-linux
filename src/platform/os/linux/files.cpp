#include "platform/os/files.h"

#include <sys/stat.h>

namespace fl::os {

bool path_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int open_file_budget() { return 512; }  // the default soft limit is 1024

}  // namespace fl::os
