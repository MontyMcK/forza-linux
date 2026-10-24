// Maps the game's Windows paths onto the Linux filesystem.
//
//   relative paths, "G:\", "D:\", "game:\"  ->  --game-dir   (read-only package)
//   "T:\" (temp), "S:\", "U:\" etc.          ->  data_dir/drive_<x>/
//
// Lookups are case-insensitive, because the game's casing need not match the dump.
// TODO(RE): confirm which drive letters the game actually uses (FINDINGS §5).
#pragma once

#include <string>

namespace fl::vfs {

// Translate a Windows path to a host path. If `for_create` is set, missing
// final components are allowed (the parent directory must exist or be creatable).
std::string to_host(const std::string& win_path, bool for_create = false);

}  // namespace fl::vfs
