// The game's one window (SDL3). SDL video is started and the window created on
// the game's UI thread (the thread running CoreApplication), which also pumps
// the events, before the render thread brings up DXVK.
#pragma once

struct SDL_Window;

namespace fl::platform {

void video_init();          // idempotent; also creates the window
SDL_Window* window();       // created on first call
void pump_events();         // called from CoreDispatcher::ProcessEvents
void window_size(int* w, int* h);
float refresh_rate();       // of the window's display, in Hz (0 if unknown)

}  // namespace fl::platform
