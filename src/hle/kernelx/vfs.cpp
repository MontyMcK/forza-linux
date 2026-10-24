#include "hle/kernelx/vfs.h"

#include <cctype>
#include <dirent.h>
#include <filesystem>
#include <mutex>
#include <strings.h>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

#include "common/config.h"
#include "common/log.h"

namespace fl::vfs {
namespace {

std::mutex g_cache_mutex;
std::unordered_map<std::string, std::string> g_cache;  // lowercase host path -> real host path

bool exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

// Find `name` inside directory `dir` ignoring case. Returns "" if absent.
std::string match_ci(const std::string& dir, const std::string& name) {
    std::string direct = dir + "/" + name;
    if (exists(direct)) return direct;
    DIR* d = opendir(dir.c_str());
    if (!d) return "";
    std::string found;
    while (dirent* e = readdir(d)) {
        if (strcasecmp(e->d_name, name.c_str()) == 0) {
            found = dir + "/" + e->d_name;
            break;
        }
    }
    closedir(d);
    return found;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

std::string to_host(const std::string& win_path, bool for_create) {
    std::string p = win_path;
    for (char& c : p)
        if (c == '\\') c = '/';

    // Split off a drive or "name:" prefix.
    std::string root = config().game_dir;
    if (auto colon = p.find(':'); colon != std::string::npos && colon < 16 && p.find('/') > colon) {
        std::string drive = lower(p.substr(0, colon));
        p = p.substr(colon + 1);
        if (drive == "g" || drive == "d" || drive == "game") {
            root = config().game_dir;
        } else {
            root = config().data_dir + "/drive_" + drive;
            std::filesystem::create_directories(root);
            FL_LOG(file, "drive '%s:' mapped to %s", drive.c_str(), root.c_str());
        }
    }

    // Normalize components (drop "", ".", resolve "..").
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find('/', start);
        if (end == std::string::npos) end = p.size();
        std::string comp = p.substr(start, end - start);
        if (comp == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!comp.empty() && comp != ".") {
            parts.push_back(comp);
        }
        start = end + 1;
    }

    std::string key = lower(root);
    for (auto& c : parts) key += "/" + lower(c);
    {
        std::lock_guard lock(g_cache_mutex);
        if (auto it = g_cache.find(key); it != g_cache.end() && exists(it->second)) return it->second;
    }

    std::string cur = root;
    for (size_t i = 0; i < parts.size(); i++) {
        std::string next = match_ci(cur, parts[i]);
        if (next.empty()) {
            // Missing component: keep the remaining path as given.
            for (size_t k = i; k < parts.size(); k++) cur += "/" + parts[k];
            return cur;
        }
        cur = next;
    }
    std::lock_guard lock(g_cache_mutex);
    g_cache[key] = cur;
    return cur;
}

}  // namespace fl::vfs
