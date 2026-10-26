#pragma once

#include <string>
#include <vector>

namespace fl {

struct Config {
    std::string game_dir;  // the dump's Mount/ directory (read-only game files)
    std::string exe_path;  // game executable inside game_dir
    std::string data_dir;  // writable: saves, settings, caches (~/.local/share/forza-linux)
    // The game's own command-line options (argv[1..] of its main), e.g.
    // engine switches like "unlockframerate" (FINDINGS §6c).
    std::vector<std::string> game_args;
    // FL_FRAME_PACING: console (default, 30 fps at the console's speed),
    // dynamic (real-time update step, any frame rate) or off (unpaced).
    enum class Pacing { console, dynamic, off } pacing = Pacing::console;
    int fps_cap = 0;  // FL_FPS_CAP: dynamic mode's frame cap, 0 = the monitor's refresh
};

Config& config();

}  // namespace fl
