#include "loader/modules.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <strings.h>
#include <sys/stat.h>

#include "common/log.h"
#include "common/winabi.h"
#include "loader/imports.h"
#include "loader/pe_structs.h"

namespace fl::loader {
namespace {

constexpr int kMaxModules = 32;
PeImage* g_modules[kMaxModules];
std::atomic<int> g_count{0};
std::recursive_mutex g_load_mutex;  // DllMain may load more modules

std::string base_name(const std::string& p) {
    auto s = p.find_last_of("\\/");
    return s == std::string::npos ? p : p.substr(s + 1);
}

}  // namespace

PeImage* image_for(const void* addr) {
    PeImage& m = main_image();
    if (m.base && m.contains(addr)) return &m;
    int n = g_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++)
        if (g_modules[i]->contains(addr)) return g_modules[i];
    return nullptr;
}

PeImage* module_by_base(const void* base) {
    int n = g_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++)
        if (g_modules[i]->base == base) return g_modules[i];
    return nullptr;
}

void* pe_export(const PeImage& img, const std::string& name) {
    auto* dos = img.at<ImageDosHeader>(0);
    auto* nt = img.at<ImageNtHeaders64>(dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[0];
    if (!dir.VirtualAddress) return nullptr;
    struct ExportDir {
        uint32_t flags, time;
        uint16_t major, minor;
        uint32_t name, base, n_functions, n_names, functions, names, ordinals;
    };
    auto* e = img.at<ExportDir>(dir.VirtualAddress);
    auto* funcs = img.at<uint32_t>(e->functions);
    if (!name.empty() && name[0] == '#') {
        uint32_t ord = static_cast<uint32_t>(std::stoul(name.substr(1)));
        if (ord < e->base || ord - e->base >= e->n_functions) return nullptr;
        return img.base + funcs[ord - e->base];
    }
    auto* names = img.at<uint32_t>(e->names);
    auto* ords = img.at<uint16_t>(e->ordinals);
    for (uint32_t i = 0; i < e->n_names; i++)
        if (name == img.at<char>(names[i])) {
            uint32_t rva = funcs[ords[i]];
            if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size)
                fatal("%s!%s is a forwarder (not supported)", img.path.c_str(), name.c_str());
            return img.base + rva;
        }
    return nullptr;
}

PeImage* load_pe_module(const std::string& path) {
    std::lock_guard lock(g_load_mutex);
    std::string bn = base_name(path);
    int n = g_count.load();
    for (int i = 0; i < n; i++)
        if (strcasecmp(base_name(g_modules[i]->path).c_str(), bn.c_str()) == 0) return g_modules[i];
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return nullptr;
    if (n == kMaxModules) fatal("too many PE modules");

    auto* img = new PeImage(map_image(path));
    if (img->tls_dir_rva) fatal("%s has static TLS (not supported for DLLs yet)", path.c_str());
    int missing = resolve_imports(*img);
    protect_image(*img);
    g_modules[n] = img;
    g_count.store(n + 1, std::memory_order_release);  // visible to the unwinder before any of its code runs
    log_write("loader", "loaded %s at %p (%d unimplemented imports)", bn.c_str(), img->base, missing);

    if (img->entry_rva) {
        using DllMain = BOOL(WINAPI*)(void*, DWORD, void*);
        BOOL ok = reinterpret_cast<DllMain>(img->base + img->entry_rva)(img->base, 1 /* DLL_PROCESS_ATTACH */, nullptr);
        log_write("loader", "%s DllMain(PROCESS_ATTACH) -> %d", bn.c_str(), ok);
        if (!ok) fatal("%s: DllMain failed", bn.c_str());
    }
    return img;
}

}  // namespace fl::loader
