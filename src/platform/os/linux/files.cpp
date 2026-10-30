#include "platform/os/files.h"

#include <sys/stat.h>

namespace fl::os {

bool path_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

}  // namespace fl::os
