// Process start and end.
#pragma once

#include <string>
#include <vector>

namespace fl::os {

// Arguments to use when the process was started with none: on the PS5 a
// title launched from the home screen gets no arguments, so it reads the
// last ones tools/ps5/run_title.sh (later the launcher) saved in its folder.
// Linux: none.
std::vector<std::string> saved_args();

// Ends the process now (no atexit handlers: game threads may still run).
// A PS5 title can't end itself (the console reports SIGSYS): it logs why and
// waits to be closed from the home screen.
[[noreturn]] void exit_process(int code);

// Ends the process after a crash or fatal error. The PS5 aborts, so the
// console's crash report shows a crash rather than a misleading SIGSYS.
[[noreturn]] void exit_now(int code);

}  // namespace fl::os
