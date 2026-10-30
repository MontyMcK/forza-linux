#include "platform/os/process.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <unistd.h>

#include "common/log.h"

namespace fl::os {

// The title's folder is /app0; run_title.sh writes one argument per line.
std::vector<std::string> saved_args() {
    std::vector<std::string> args;
    std::ifstream f("/app0/launch-args.txt");
    for (std::string line; std::getline(f, line);)
        if (!line.empty()) args.push_back(line);
    return args;
}

void exit_process(int code) {
    log_write("loader", "the runtime ended (code %d); a PS5 title can't close itself: close it from the home screen", code);
    fflush(nullptr);
    for (;;) sleep(3600);
}

void exit_now(int) { abort(); }

}  // namespace fl::os
