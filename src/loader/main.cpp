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
#include "loader/cpu_profile.h"
#include "loader/crash.h"
#include "loader/game_patches.h"
#include "loader/imports.h"
#include "loader/pe_image.h"
#include "loader/thread_env.h"
#include "platform/os/thread.h"
#include "platform/os/process.h"
#include "platform/os/window.h"

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
            "usage: %s --game-dir <path to the dump's Mount/ folder> [--exe <name>] [--env NAME=VALUE]... [-- <game options>...]\n"
            "env: FL_LOG=all|chan,...  FL_LOG_FILE=path  FL_TRACE_IMPORTS=1|all\n"
            "     FL_FRAME_PACING=console|dynamic|off  FL_FPS_CAP=<fps> (dynamic)\n",
            argv0);
    os::exit_process(2);
}

// --env NAME=VALUE (repeatable) sets an environment variable before anything
// reads one: a PS5 title has no environment, so its launcher passes FL_LOG_FILE
// and the other settings this way.
void apply_env_args(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--env") != 0) continue;
        std::string kv = argv[++i];
        size_t eq = kv.find('=');
        if (eq != std::string::npos) setenv(kv.substr(0, eq).c_str(), kv.c_str() + eq + 1, 1);
    }
}

void parse_args(int argc, char** argv) {
    Config& c = config();
    std::string exe = kDefaultExe;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game-dir") && i + 1 < argc)
            c.game_dir = argv[++i];
        else if (!strcmp(argv[i], "--exe") && i + 1 < argc)
            exe = argv[++i];
        else if (!strcmp(argv[i], "--env") && i + 1 < argc)
            ++i;  // applied by apply_env_args()
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
    void* stack_lo;
    size_t stack_size;
    os::current_stack(&stack_lo, &stack_size);

    loader::attach_thread(stack_lo, static_cast<uint8_t*>(stack_lo) + stack_size, /*main_thread=*/true);
    auto& img = loader::main_image();
    auto entry = reinterpret_cast<EntryPoint>(img.base + img.entry_rva);
    loader::arm_breakpoints();
    log_write("loader", "jumping to game entry point %p", reinterpret_cast<void*>(entry));
    int rc = entry();
    log_write("loader", "game entry point returned %d", rc);
    os::exit_process(rc);
}

}  // namespace
}  // namespace fl

int main(int argc, char** argv) {
    using namespace fl;
    // Started with no arguments (a PS5 title from the home screen): use the
    // saved ones (os::saved_args).
    std::vector<std::string> saved;
    std::vector<char*> saved_argv;
    if (argc <= 1 && !(saved = os::saved_args()).empty()) {
        saved_argv.push_back(argv[0]);
        for (std::string& a : saved) saved_argv.push_back(a.data());
        saved_argv.push_back(nullptr);
        argc = static_cast<int>(saved_argv.size()) - 1;
        argv = saved_argv.data();
    }
    apply_env_args(argc, argv);
    // dxvk-native refuses to start without a WSI choice. Set it before any
    // game thread exists (setenv is not thread-safe).
    setenv("DXVK_WSI_DRIVER", os::dxvk_wsi_driver(), 0);
    log_init();
    parse_args(argc, argv);
    // DXVK keeps translated shaders under XDG_CACHE_HOME or HOME; a PS5
    // title has neither, so it would translate every shader again each run.
    if (!getenv("DXVK_SHADER_CACHE_PATH") && !getenv("XDG_CACHE_HOME") && !getenv("HOME"))
        setenv("DXVK_SHADER_CACHE_PATH", (config().data_dir + "/dxvk-cache").c_str(), 0);
    loader::install_crash_handler();
    log_write("loader", "game dir: %s", config().game_dir.c_str());
    log_write("loader", "data dir: %s", config().data_dir.c_str());

    loader::main_image() = loader::map_image(config().exe_path);
    loader::apply_game_patches(loader::main_image());
    int missing = loader::resolve_imports(loader::main_image());
    if (missing) log_write("loader", "%d imports are unimplemented traps (they abort when called)", missing);
    loader::protect_image(loader::main_image());
    loader::init_process();
    loader::start_cpu_profiler();

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kMainStackSize);
    pthread_t t;
    if (pthread_create(&t, &attr, game_main_thread, nullptr) != 0) fatal("pthread_create failed");
    pthread_join(t, nullptr);
    return 0;
}
