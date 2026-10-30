// PS5 display: DXVK presents through PS5_Vulkan's VK_KHR_display on VideoOut
// (PS5_PORT.md Step 5/6), not written yet.
#include "platform/os/window.h"

#include "common/log.h"

namespace fl::os {

void video_init() { FL_STUB_ONCE("os::video_init on PS5: nothing to do until the VideoOut swap chain exists"); }

void* window() {
    FL_ERROR("UNIMPLEMENTED os::window on PS5 (VK_KHR_display on VideoOut, PS5_PORT.md M2)");
    return nullptr;
}

void pump_events() {}

void window_size(int* w, int* h) {
    *w = 1920;  // STUB: VideoOut's mode once the display exists
    *h = 1080;
}

float refresh_rate() { return 0; }  // unknown until VideoOut reports it

}  // namespace fl::os
