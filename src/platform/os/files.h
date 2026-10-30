// File-system queries that differ between hosts.
#pragma once

namespace fl::os {

// True if `path` exists (file or folder). Uses stat(): access() fails with
// EPERM for every path inside a PS5 title.
bool path_exists(const char* path);

}  // namespace fl::os
