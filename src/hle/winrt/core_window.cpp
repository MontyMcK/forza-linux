#include "hle/winrt/core_window.h"

#include <sched.h>
#include <utility>

#include "common/log.h"
#include "hle/winrt/activation.h"
#include "platform/window.h"

namespace fl::winrt {
namespace {

constexpr GUID IID_ICoreWindow = {0x79B9D5F2, 0x879E, 0x4B89, {0xB7, 0x98, 0x79, 0xE4, 0x75, 0x98, 0x03, 0x0C}};
constexpr GUID IID_ICoreDispatcher = {0x60DB2FA8, 0xB705, 0x4FDE, {0xA7, 0xD6, 0xEB, 0xBB, 0x18, 0x91, 0xD3, 0x9E}};
constexpr GUID IID_ICoreWindowStatic = {0x4D239005, 0x3C2A, 0x41B1, {0x90, 0x22, 0x53, 0x6B, 0xB9, 0xCF, 0x93, 0xB1}};

constexpr int S = kInspectableSlots;

// Xbox ICoreWindow event pairs (S+17 + 2*i). Verified from the delegate types
// the game registers (rva 0xf6c746/0xf6c7fd: KeyEventArgs, 0xf6c5db:
// VisibilityChangedEventArgs). The middle four are not identified yet.
enum CoreWindowEvent { kKeyDown = 0, kKeyUp = 1, kVisibilityChanged = 6 };
const char* const kEventNames[kCoreWindowEventPairs] = {"KeyDown", "KeyUp", "event#2", "event#3",
                                                        "event#4", "event#5", "VisibilityChanged"};

CoreWindow* self_of(Iface* i) { return i->as<CoreWindow>(); }

// ---------------------------------------------------------------- ICoreWindow

[[maybe_unused]] WINAPI HRESULT cw_get_Bounds(Iface* self, Rect* out) {
    *out = self_of(self)->bounds;
    return S_OK;
}

WINAPI HRESULT cw_get_Dispatcher(Iface* self, void** out) {
    self_of(self)->add_ref();
    *out = &self_of(self)->idispatcher;
    return S_OK;
}

[[maybe_unused]] WINAPI HRESULT cw_get_Visible(Iface* self, BOOLEAN* out) {
    *out = self_of(self)->visible;
    return S_OK;
}

WINAPI HRESULT cw_Activate(Iface* self) {
    fl::log_write("winrt", "CoreWindow::Activate");
    platform::window();  // the real window appears now, on the UI thread
    self_of(self)->activated = true;
    self_of(self)->visible = true;
    return S_OK;
}

[[maybe_unused]] WINAPI HRESULT cw_get_PointerCursor(Iface*, void** out) {
    *out = nullptr;
    return S_OK;
}

// The game hides the cursor (put_PointerCursor(null)) at rva 0xf6c695.
WINAPI HRESULT cw_put_PointerCursor(Iface*, void* cursor) {
    FL_LOG(winrt, "CoreWindow::put_PointerCursor(%p)", cursor);
    return S_OK;
}

[[maybe_unused]] WINAPI HRESULT cw_get_IsInputEnabled(Iface* self, BOOLEAN* out) {
    *out = 1;
    return S_OK;
}

template <int N>
WINAPI HRESULT cw_add_event(Iface* self, void* handler, EventToken* token) {
    FL_LOG(winrt, "CoreWindow::add_%s", kEventNames[N]);
    return self_of(self)->events[N].add(handler, token);
}

template <int N>
WINAPI HRESULT cw_remove_event(Iface* self, EventToken token) {
    return self_of(self)->events[N].remove(token);
}

template <int... N>
void set_events(Vtable& v, std::integer_sequence<int, N...>) {
    ((v.set(S + kCoreWindowFirstEvent + 2 * N, &cw_add_event<N>),
      v.set(S + kCoreWindowFirstEvent + 1 + 2 * N, &cw_remove_event<N>)),
     ...);
}

Vtable& window_vt() {
    static Vtable v = [] {
        // Only slots verified from game call sites (FINDINGS §4b). Guessing the
        // desktop order corrupts memory: S+1 is get_Dispatcher here, not get_Bounds.
        Vtable t("ICoreWindow(Xbox)", S + 40);
        t.set(S + 1, &cw_get_Dispatcher)   // rva 0xf74a5e: result gets ProcessEvents(3)
            .set(S + 5, &cw_Activate)      // rva 0xf6c858: no args
            .set(S + 9, &cw_put_PointerCursor);  // rva 0xf6c692: CoreCursor arg
        set_events(t, std::make_integer_sequence<int, kCoreWindowEventPairs>{});
        return t;
    }();
    return v;
}

// ---------------------------------------------------------------- ICoreDispatcher

WINAPI HRESULT cd_get_HasThreadAccess(Iface*, BOOLEAN* out) {
    *out = 1;
    return S_OK;
}

// CoreProcessEventsOption: 0 ProcessOneIfPresent, 1 ProcessUntilQuit, 2 ProcessAllIfPresent, 3 ProcessOneAndAllPending
WINAPI HRESULT cd_ProcessEvents(Iface* self, int option) {
    static bool logged = false;
    if (!logged) {
        logged = true;
        fl::log_write("winrt", "CoreDispatcher::ProcessEvents(%d): the game loop is pumping events", option);
    }
    platform::pump_events();
    if (option == 1) fl::fatal("ProcessEvents(ProcessUntilQuit) not supported yet");
    return S_OK;
}

Vtable& dispatcher_vt() {
    static Vtable v = Vtable("ICoreDispatcher", S + 4).set(S + 0, &cd_get_HasThreadAccess).set(S + 1, &cd_ProcessEvents);
    return v;
}

// ---------------------------------------------------------------- CoreWindow statics

struct CoreWindowStatics : Object {
    static WINAPI HRESULT GetForCurrentThread(Iface*, void** out) {
        *out = CoreWindow::instance().default_iface();
        return S_OK;
    }
    static Vtable& vt() {
        static Vtable v = Vtable("ICoreWindowStatic", S + 1).set(S + 0, &GetForCurrentThread);
        return v;
    }
    Iface istatic{vt(), this};
    CoreWindowStatics() : Object(u"Windows.UI.Core.CoreWindow") { expose(IID_ICoreWindowStatic, &istatic); }
};

HRESULT get_core_window_statics(const GUID& iid, void** out) {
    static CoreWindowStatics* s = new CoreWindowStatics();
    return s->query(iid, out);
}

}  // namespace

CoreWindow::CoreWindow()
    : Object(u"Windows.UI.Core.CoreWindow"), iwindow(window_vt(), this), idispatcher(dispatcher_vt(), this) {
    expose(IID_ICoreWindow, &iwindow);
    expose(IID_ICoreDispatcher, &idispatcher);
    // Start SDL video here, on the UI thread, before the render thread starts DXVK.
    platform::video_init();
}

CoreWindow& CoreWindow::instance() {
    static CoreWindow* w = new CoreWindow();
    return *w;
}

WINRT_REGISTER_CLASS(u"Windows.UI.Core.CoreWindow", get_core_window_statics);

}  // namespace fl::winrt
