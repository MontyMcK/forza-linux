// forza-linux entry point: map the game exe, bind imports, run it.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <pthread.h>
#include <string>
#include <sys/mman.h>

#include "common/config.h"
#include "common/log.h"
#include "loader/crash.h"
#include "loader/game_patches.h"
#include "loader/imports.h"
#include "loader/pe_image.h"
#include "loader/thread_env.h"

namespace fl {

Config& config() {
    static Config c;
    return c;
}

namespace {

constexpr const char* kDefaultExe = "forza_durango_release_final.exe";
constexpr size_t kMainStackSize = 64ull << 20;  // the game's main thread does heavy init

void usage(const char* argv0) {
    fprintf(stderr,
            "usage: %s --game-dir <path to the dump's Mount/ folder> [--exe <name>] [-- <game options>...]\n"
            "env: FL_LOG=all|chan,...  FL_LOG_FILE=path  FL_TRACE_IMPORTS=1|all\n"
            "     FL_FRAME_PACING=console|dynamic|off  FL_FPS_CAP=<fps> (dynamic)\n",
            argv0);
    exit(2);
}

void parse_args(int argc, char** argv) {
    Config& c = config();
    std::string exe = kDefaultExe;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game-dir") && i + 1 < argc)
            c.game_dir = argv[++i];
        else if (!strcmp(argv[i], "--exe") && i + 1 < argc)
            exe = argv[++i];
        else if (!strcmp(argv[i], "--")) {
            for (i++; i < argc; i++) c.game_args.push_back(argv[i]);
        } else
            usage(argv[0]);
    }
    // Frame pacing (FINDINGS §6c). The engine steps its update by a fixed
    // 1/29.97 s per frame while synchroniseUpdateToRenderThread is on (the
    // console default); with it off the step is the measured frame time
    // (clamped to 0.2 s), and unlockframerate drops the engine's own limiter.
    if (const char* p = getenv("FL_FRAME_PACING")) {
        if (!strcmp(p, "dynamic"))
            c.pacing = Config::Pacing::dynamic;
        else if (!strcmp(p, "off"))
            c.pacing = Config::Pacing::off;
        else if (strcmp(p, "console") != 0)
            fatal("FL_FRAME_PACING=%s: use console, dynamic or off", p);
    }
    if (const char* f = getenv("FL_FPS_CAP")) c.fps_cap = atoi(f);
    if (c.game_dir.empty()) usage(argv[0]);
    c.game_dir = std::filesystem::absolute(c.game_dir).lexically_normal().string();
    if (!c.game_dir.empty() && c.game_dir.back() == '/') c.game_dir.pop_back();
    c.exe_path = c.game_dir + "/" + exe;

    const char* xdg = getenv("XDG_DATA_HOME");
    const char* home = getenv("HOME");
    c.data_dir = xdg ? std::string(xdg) + "/forza-linux" : std::string(home ? home : "/tmp") + "/.local/share/forza-linux";
    std::filesystem::create_directories(c.data_dir);
}

using EntryPoint = int(WINAPI*)();

void* game_main_thread(void*) {
    pthread_attr_t attr;
    void* stack_lo;
    size_t stack_size;
    pthread_getattr_np(pthread_self(), &attr);
    pthread_attr_getstack(&attr, &stack_lo, &stack_size);
    pthread_attr_destroy(&attr);

    loader::attach_thread(stack_lo, static_cast<uint8_t*>(stack_lo) + stack_size, /*main_thread=*/true);
    auto& img = loader::main_image();
    auto entry = reinterpret_cast<EntryPoint>(img.base + img.entry_rva);
    loader::arm_breakpoints();
    log_write("loader", "jumping to game entry point %p", reinterpret_cast<void*>(entry));
    int rc = entry();
    log_write("loader", "game entry point returned %d", rc);
    exit(rc);
}

}  // namespace
}  // namespace fl

int main(int argc, char** argv) {
    using namespace fl;
    // dxvk-native refuses to start without a WSI choice. Set it before any
    // game thread exists (setenv is not thread-safe).
    setenv("DXVK_WSI_DRIVER", "SDL3", 0);
    log_init();
    parse_args(argc, argv);
    loader::install_crash_handler();
    log_write("loader", "game dir: %s", config().game_dir.c_str());
    log_write("loader", "data dir: %s", config().data_dir.c_str());

    loader::main_image() = loader::map_image(config().exe_path);
    loader::apply_game_patches(loader::main_image());
    int missing = loader::resolve_imports(loader::main_image());
    if (missing) log_write("loader", "%d imports are unimplemented traps (they abort when called)", missing);
    loader::protect_image(loader::main_image());
    loader::init_process();

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kMainStackSize);
    pthread_t t;
    if (pthread_create(&t, &attr, game_main_thread, nullptr) != 0) fatal("pthread_create failed");
    pthread_join(t, nullptr);
    return 0;
}
