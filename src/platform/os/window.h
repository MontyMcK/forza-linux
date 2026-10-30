// The game's one window (Linux: SDL3). Video is started and the window created
// on the game's UI thread (the thread running CoreApplication), which also
// pumps the events, before the render thread brings up DXVK.
#pragma once


namespace fl::os {

void video_init();          // idempotent; also creates the window
void* window();             // created on first call; the handle DXVK's swap chain is made for (Linux: SDL_Window*)
void pump_events();         // called from CoreDispatcher::ProcessEvents
void window_size(int* w, int* h);
float refresh_rate();       // of the window's display, in Hz (0 if unknown)

}  // namespace fl::os
