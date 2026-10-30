// PS5 display: DXVK presents through PS5_Vulkan's VK_KHR_display on VideoOut
// (PS5_PORT.md Step 5/6), not written yet.
#include "platform/os/window.h"

#include "common/log.h"

namespace fl::os {

void video_init() { FL_STUB_ONCE("os::video_init on PS5: nothing to do until the VideoOut swap chain exists"); }

// DXVK's PS5 WSI presents on the whole display and ignores the handle, but
// refuses a null one: any stable non-null value will do.
void* window() {
    static char display;
    return &display;
}

void pump_events() {}

void window_size(int* w, int* h) {
    *w = 1920;  // STUB: VideoOut's mode once the display exists
    *h = 1080;
}

float refresh_rate() { return 0; }  // unknown until VideoOut reports it

const char* dxvk_wsi_driver() { return "PS5"; }

}  // namespace fl::os
