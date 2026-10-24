#pragma once

#include <string>

namespace fl {

struct Config {
    std::string game_dir;  // the dump's Mount/ directory (read-only game files)
    std::string exe_path;  // game executable inside game_dir
    std::string data_dir;  // writable: saves, settings, caches (~/.local/share/forza-linux)
};

Config& config();

}  // namespace fl
