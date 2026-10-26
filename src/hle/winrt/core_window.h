// Windows.UI.Core.CoreWindow and CoreDispatcher.
// One window per process. Headless for now; it will be backed by an SDL3
// window when graphics land (Phase 5).
#pragma once

#include "hle/winrt/object.h"

namespace fl::winrt {

struct Rect {
    float X, Y, Width, Height;
};

// The Xbox ICoreWindow is NOT the desktop layout (see FINDINGS §4b). Event
// add_/remove_ pairs start at S+17; each gets a generic EventSource indexed by
// (slot - 17) / 2.
constexpr int kCoreWindowFirstEvent = 17;
constexpr int kCoreWindowEventPairs = 7;

struct CoreWindow : Object {
    static CoreWindow& instance();

    CoreWindow();
    Iface iwindow;
    Iface idispatcher;
    EventSource events[kCoreWindowEventPairs];
    bool visible = false;
    bool activated = false;
    bool visibility_raised = false;  // VisibilityChanged(true) sent after activation
    bool activation_raised = false;  // Activated(CodeActivated) sent after activation
    EventSource activated_event;     // Xbox ICoreWindow S+9/S+10
    Rect bounds{0, 0, 1920, 1080};
};

}  // namespace fl::winrt
