#include "platform/window.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
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
    g_window = SDL_CreateWindow("Forza Horizon 2 (forza-linux)", w, h, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!g_window) fatal("SDL_CreateWindow failed: %s", SDL_GetError());
    log_write("platform", "window %dx%d created", w, h);
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
    }
}

}  // namespace fl::platform
