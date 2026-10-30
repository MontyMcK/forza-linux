// File-system queries that differ between hosts.
#pragma once

namespace fl::os {

// True if `path` exists (file or folder). Uses stat(): access() fails with
// EPERM for every path inside a PS5 title.
bool path_exists(const char* path);

// How many read-only game files the runtime keeps open at once; the rest are
// reopened when used. The game holds ~420 open (Linux, title screen), more
// than a PS5 title may (250 descriptors in all, FINDINGS 7.5).
int open_file_budget();

}  // namespace fl::os
