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

// FL_DISPLAY_MODE=windowed|borderless|fullscreen (FL_FULLSCREEN=1, the old
// switch, means borderless).
//   borderless: a borderless window covering the display at its desktop mode;
//   fullscreen: exclusive fullscreen in a display mode: the closest one to
//               FL_WINDOW if that is set, else the desktop mode. (On Wayland
//               SDL emulates a smaller mode by scaling; the output's own mode
//               and refresh rate stay as they are.)
enum class Mode { windowed, borderless, fullscreen };

Mode configured_mode() {
    if (const char* m = getenv("FL_DISPLAY_MODE")) {
        if (!strcmp(m, "windowed")) return Mode::windowed;
        if (!strcmp(m, "borderless")) return Mode::borderless;
        if (!strcmp(m, "fullscreen")) return Mode::fullscreen;
        FL_ERROR("FL_DISPLAY_MODE=%s: expected windowed, borderless or fullscreen (using windowed)", m);
        return Mode::windowed;
    }
    const char* fs = getenv("FL_FULLSCREEN");
    return fs && strcmp(fs, "0") != 0 ? Mode::borderless : Mode::windowed;
}

const char* mode_name(Mode m) {
    return m == Mode::windowed ? "windowed" : m == Mode::borderless ? "borderless" : "fullscreen";
}

void set_mode(Mode m) {
    if (m == Mode::windowed) {
        if (!SDL_SetWindowFullscreen(g_window, false)) FL_ERROR("leaving fullscreen failed: %s", SDL_GetError());
        else log_write("platform", "display mode: windowed");
        return;
    }
    const SDL_DisplayMode* use = nullptr;  // null: borderless at the desktop mode
    SDL_DisplayMode closest{};
    if (m == Mode::fullscreen) {
        SDL_DisplayID d = SDL_GetDisplayForWindow(g_window);
        const SDL_DisplayMode* desk = d ? SDL_GetDesktopDisplayMode(d) : nullptr;
        int w = desk ? desk->w : 0, h = desk ? desk->h : 0;
        if (getenv("FL_WINDOW")) {
            int ww, wh;
            parse_size(&ww, &wh);
            w = ww, h = wh;
        }
        if (d && SDL_GetClosestFullscreenDisplayMode(d, w, h, desk ? desk->refresh_rate : 0.0f, false, &closest))
            use = &closest;
        else
            FL_ERROR("no %dx%d fullscreen mode (%s): using borderless", w, h, SDL_GetError());
    }
    if (!SDL_SetWindowFullscreenMode(g_window, use) || !SDL_SetWindowFullscreen(g_window, true)) {
        FL_ERROR("%s failed: %s", mode_name(m), SDL_GetError());
        return;
    }
    if (use)
        log_write("platform", "display mode: fullscreen %dx%d @ %.2f Hz", use->w, use->h, use->refresh_rate);
    else
        log_write("platform", "display mode: borderless");
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
    // Starts in the configured display mode (configured_mode); F11 or Alt+Enter
    // switches between windowed and it while playing.
    // Borderless and fullscreen start on the primary display (a new window
    // would otherwise go wherever the compositor places it, e.g. the monitor
    // under the mouse).
    Mode mode = configured_mode();
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | (mode != Mode::windowed ? SDL_WINDOW_HIDDEN : 0);
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "Forza Horizon 2 (forza-linux)");
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, w);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, h);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, flags);
    if (mode != Mode::windowed) {
        SDL_DisplayID primary = SDL_GetPrimaryDisplay();
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(primary));
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(primary));
    }
    g_window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!g_window) fatal("SDL_CreateWindow failed: %s", SDL_GetError());
    log_write("platform", "window %dx%d created (%s)", w, h, mode_name(mode));
    if (mode != Mode::windowed) {
        set_mode(mode);
        SDL_ShowWindow(g_window);
    }
    log_refresh();
    return g_window;
}

float refresh_rate() {
    if (!g_window) return 0;
    SDL_DisplayID d = SDL_GetDisplayForWindow(g_window);
    const SDL_DisplayMode* m = d ? SDL_GetCurrentDisplayMode(d) : nullptr;
    return m ? m->refresh_rate : 0;
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
            (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT)))) {
            static const Mode chosen = configured_mode() == Mode::windowed ? Mode::borderless : configured_mode();
            set_mode(SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN ? Mode::windowed : chosen);
        }
        if (e.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED) log_refresh();
    }
}

}  // namespace fl::platform
