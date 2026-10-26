#include "platform/window.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unistd.h>

#include "common/log.h"

namespace fl::platform {
namespace {

std::once_flag g_init_once;
std::mutex g_window_mutex;
SDL_Window* g_window = nullptr;

void parse_size(int* w, int* h) {
    *w = 1280;
    *h = 720;
    if (const char* s = getenv("FL_WINDOW")) std::sscanf(s, "%dx%d", w, h);
}

// The game shows a new frame every 1/29.97 s. On a display whose refresh is
// not a multiple of ~30 Hz (144, 165, 260 Hz) frames stay up for alternating
// numbers of refreshes, which looks like judder even at a steady 30 fps.
void log_refresh() {
    SDL_DisplayID d = SDL_GetDisplayForWindow(g_window);
    const SDL_DisplayMode* m = d ? SDL_GetCurrentDisplayMode(d) : nullptr;
    if (!m || m->refresh_rate <= 0) return;
    float hz = m->refresh_rate;
    float per_frame = hz / 29.97f;
    bool even = std::fabs(per_frame - std::round(per_frame)) < 0.05f;
    log_write("platform", "display %s: %.2f Hz (%.2f refreshes per game frame)%s", SDL_GetDisplayName(d), hz,
              per_frame, even ? "" : ": frames will judder; 60/120/240 Hz or variable refresh (fullscreen) is smooth");
}

void set_fullscreen(bool on) {
    if (!SDL_SetWindowFullscreen(g_window, on))
        FL_ERROR("fullscreen %s failed: %s", on ? "on" : "off", SDL_GetError());
    else
        log_write("platform", "fullscreen %s", on ? "on" : "off");
}

}  // namespace

void video_init() {
    std::call_once(g_init_once, [] {
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
            fatal("SDL_InitSubSystem(VIDEO | GAMEPAD) failed: %s", SDL_GetError());
        // Load Vulkan now, before the render thread starts: DXVK's SDL3 WSI
        // loads it too, and SDL's backend loader is not safe against a
        // concurrent first load (it fails with "Vulkan already loaded").
        if (!SDL_Vulkan_LoadLibrary(nullptr)) fatal("SDL_Vulkan_LoadLibrary failed: %s", SDL_GetError());
        log_write("platform", "SDL %d.%d.%d, video driver %s", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
                  SDL_MICRO_VERSION, SDL_GetCurrentVideoDriver());
    });
    window();
}

SDL_Window* window() {
    std::lock_guard lock(g_window_mutex);
    if (g_window) return g_window;
    int w, h;
    parse_size(&w, &h);
    // FL_FULLSCREEN=1: borderless fullscreen on the window's display (F11 or
    // Alt+Enter toggles it while playing).
    const char* fs = getenv("FL_FULLSCREEN");
    bool fullscreen = fs && strcmp(fs, "0") != 0;
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
    g_window = SDL_CreateWindow("Forza Horizon 2 (forza-linux)", w, h, flags);
    if (!g_window) fatal("SDL_CreateWindow failed: %s", SDL_GetError());
    log_write("platform", "window %dx%d created%s", w, h, fullscreen ? " (fullscreen)" : "");
    log_refresh();
    return g_window;
}

void window_size(int* w, int* h) {
    if (!g_window || !SDL_GetWindowSizeInPixels(g_window, w, h)) parse_size(w, h);
}

void pump_events() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            log_write("platform", "window closed: exiting");
            _exit(0);
        }
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
            (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT))))
            set_fullscreen(!(SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN));
        if (e.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED) log_refresh();
    }
}

}  // namespace fl::platform
