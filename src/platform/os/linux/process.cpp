#include "platform/os/process.h"

#include <cstdio>
#include <unistd.h>

#include "common/log.h"

namespace fl::os {

std::vector<std::string> saved_args() { return {}; }

void exit_process(int code) {
    log_flush();
    fflush(nullptr);
    _exit(code);
}

void exit_now(int code) { _exit(code); }

}  // namespace fl::os
