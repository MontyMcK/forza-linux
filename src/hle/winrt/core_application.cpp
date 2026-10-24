// Windows.ApplicationModel.Core.CoreApplication / CoreApplicationView and
// Windows.UI.Core.CoreWindow / CoreDispatcher.
//
// Interface slot orders follow the public Windows SDK metadata (Windows 8 era).
// The Xbox ERA build may differ: if a slot traps or misbehaves, verify it
// against the game's call site and note it in FINDINGS.md.

#include <chrono>
#include <thread>

#include "common/log.h"
#include "common/wstr.h"
#include "hle/winrt/activation.h"
#include "hle/winrt/core_window.h"
#include "hle/winrt/object.h"

namespace fl::winrt {
namespace {

constexpr GUID IID_ICoreApplication = {0x0AACF7A4, 0x5E1D, 0x49DF, {0x80, 0x34, 0xFB, 0x6A, 0x68, 0xBC, 0x5E, 0xD1}};
constexpr GUID IID_ICoreApplicationView = {0x638BB2DB, 0x451D, 0x4661, {0xB0, 0x99, 0x41, 0x4F, 0x34, 0xFF, 0xB9, 0xF1}};
constexpr GUID IID_IActivatedEventArgs = {0xCF651713, 0xCD08, 0x4FD8, {0xB6, 0x97, 0xA2, 0x81, 0xB6, 0x54, 0x4E, 0x2E}};
constexpr GUID IID_ILaunchActivatedEventArgs = {0xFBC93E26, 0xA14A, 0x4B4F, {0x82, 0xB0, 0x33, 0xBE, 0xD9, 0x20, 0xAF, 0x52}};

constexpr int S = kInspectableSlots;

// ---------------------------------------------------------------- activated event args

struct LaunchArgs : Object {
    static WINAPI HRESULT get_Kind(Iface*, int* kind) {
        *kind = 0;  // ActivationKind_Launch
        return S_OK;
    }
    static WINAPI HRESULT get_PreviousExecutionState(Iface*, int* state) {
        *state = 0;  // NotRunning
        return S_OK;
    }
    static WINAPI HRESULT get_SplashScreen(Iface*, void** out) {
        *out = nullptr;
        return E_NOTIMPL;
    }
    static WINAPI HRESULT get_Arguments(Iface*, HSTRING* out) {
        *out = nullptr;  // empty string
        return S_OK;
    }
    static WINAPI HRESULT get_TileId(Iface*, HSTRING* out) { return make_hstring(u"App", out); }

    static Vtable& vt_args() {
        static Vtable v = Vtable("IActivatedEventArgs", S + 3)
                              .set(S + 0, &get_Kind)
                              .set(S + 1, &get_PreviousExecutionState)
                              .set(S + 2, &get_SplashScreen);
        return v;
    }
    static Vtable& vt_launch() {
        static Vtable v = Vtable("ILaunchActivatedEventArgs", S + 2).set(S + 0, &get_Arguments).set(S + 1, &get_TileId);
        return v;
    }

    Iface iargs{vt_args(), this};
    Iface ilaunch{vt_launch(), this};
    LaunchArgs() : Object(u"Windows.ApplicationModel.Activation.LaunchActivatedEventArgs") {
        expose(IID_IActivatedEventArgs, &iargs);
        expose(IID_ILaunchActivatedEventArgs, &ilaunch);
    }
};

// ---------------------------------------------------------------- CoreApplicationView

struct CoreApplicationView : Object {
    EventSource activated;

    static WINAPI HRESULT get_CoreWindow(Iface*, void** out) {
        *out = CoreWindow::instance().default_iface();
        return S_OK;
    }
    static WINAPI HRESULT add_Activated(Iface* self, void* handler, EventToken* token) {
        FL_LOG(winrt, "CoreApplicationView::add_Activated");
        return self->as<CoreApplicationView>()->activated.add(handler, token);
    }
    static WINAPI HRESULT remove_Activated(Iface* self, EventToken token) {
        return self->as<CoreApplicationView>()->activated.remove(token);
    }
    static WINAPI HRESULT get_IsMain(Iface*, BOOLEAN* out) {
        *out = 1;
        return S_OK;
    }
    static WINAPI HRESULT get_IsHosted(Iface*, BOOLEAN* out) {
        *out = 0;
        return S_OK;
    }

    static Vtable& vt() {
        static Vtable v = Vtable("ICoreApplicationView", S + 5)
                              .set(S + 0, &get_CoreWindow)
                              .set(S + 1, &add_Activated)
                              .set(S + 2, &remove_Activated)
                              .set(S + 3, &get_IsMain)
                              .set(S + 4, &get_IsHosted);
        return v;
    }

    Iface iview{vt(), this};
    CoreApplicationView() : Object(u"Windows.ApplicationModel.Core.CoreApplicationView") {
        expose(IID_ICoreApplicationView, &iview);
    }
};

CoreApplicationView* g_main_view = nullptr;

// ---------------------------------------------------------------- Xbox resource availability
// Xbox-only statics interface {9FF3799E-6E83-468C-A5B0-501CE6BB0FA1} on
// CoreApplication (call sites rva 0x38d99c / 0x38ec70 / 0x38f264):
//   S+0 get_ResourceAvailability(int*)  (0 = Full, 1 = Constrained, i.e. snapped)
//   S+1 add_ResourceAvailabilityChanged(handler, token*)   S+2 remove_(token)
// We are never snapped, so the event never fires.
constexpr GUID IID_ICoreApplicationResourceAvailability = {0x9FF3799E, 0x6E83, 0x468C,
                                                          {0xA5, 0xB0, 0x50, 0x1C, 0xE6, 0xBB, 0x0F, 0xA1}};

// ---------------------------------------------------------------- CoreApplication statics

struct CoreApplicationStatics : Object {
    EventSource suspending, resuming, availability_changed;

    static WINAPI HRESULT get_ResourceAvailability(Iface*, int32_t* out) {
        *out = 0;  // Full
        return S_OK;
    }
    static WINAPI HRESULT add_AvailabilityChanged(Iface* self, void* h, EventToken* t) {
        return self->as<CoreApplicationStatics>()->availability_changed.add(h, t);
    }
    static WINAPI HRESULT remove_AvailabilityChanged(Iface* self, EventToken t) {
        return self->as<CoreApplicationStatics>()->availability_changed.remove(t);
    }
    static Vtable& vt_resources() {
        static Vtable v = Vtable("ICoreApplicationResourceAvailability", S + 3)
                              .set(S + 0, &get_ResourceAvailability)
                              .set(S + 1, &add_AvailabilityChanged)
                              .set(S + 2, &remove_AvailabilityChanged);
        return v;
    }

    static WINAPI HRESULT get_Id(Iface*, HSTRING* out) { return make_hstring(u"ReleaseFinal", out); }
    static WINAPI HRESULT add_Suspending(Iface* self, void* h, EventToken* t) {
        return self->as<CoreApplicationStatics>()->suspending.add(h, t);
    }
    static WINAPI HRESULT remove_Suspending(Iface* self, EventToken t) {
        return self->as<CoreApplicationStatics>()->suspending.remove(t);
    }
    static WINAPI HRESULT add_Resuming(Iface* self, void* h, EventToken* t) {
        return self->as<CoreApplicationStatics>()->resuming.add(h, t);
    }
    static WINAPI HRESULT remove_Resuming(Iface* self, EventToken t) {
        return self->as<CoreApplicationStatics>()->resuming.remove(t);
    }
    static WINAPI HRESULT GetCurrentView(Iface*, void** out) {
        *out = g_main_view->default_iface();
        return S_OK;
    }

    // The heart of app startup: drive the game's IFrameworkView.
    static WINAPI HRESULT Run(Iface*, void* view_source) {
        fl::log_write("winrt", "CoreApplication::Run: creating the game's framework view");
        void* view = nullptr;
        HRESULT hr = call_slot<HRESULT>(view_source, S + 0, &view);  // IFrameworkViewSource::CreateView
        if (hr < 0 || !view) fl::fatal("IFrameworkViewSource::CreateView failed: 0x%08x", static_cast<uint32_t>(hr));

        void* app_view = g_main_view->default_iface();
        void* window = CoreWindow::instance().default_iface();

        FL_LOG(winrt, "IFrameworkView::Initialize");
        hr = call_slot<HRESULT>(view, S + 0, app_view);
        if (hr < 0) fl::fatal("IFrameworkView::Initialize failed: 0x%08x", static_cast<uint32_t>(hr));
        FL_LOG(winrt, "IFrameworkView::SetWindow");
        hr = call_slot<HRESULT>(view, S + 1, window);
        if (hr < 0) fl::fatal("IFrameworkView::SetWindow failed: 0x%08x", static_cast<uint32_t>(hr));
        FL_LOG(winrt, "IFrameworkView::Load");
        HSTRING entry;
        make_hstring(u"Forza.App", &entry);
        hr = call_slot<HRESULT>(view, S + 2, entry);
        if (hr < 0) fl::fatal("IFrameworkView::Load failed: 0x%08x", static_cast<uint32_t>(hr));

        // Activation happens before the view's Run loop starts.
        auto* args = new LaunchArgs();
        g_main_view->activated.invoke(app_view, &args->iargs);
        args->release();

        fl::log_write("winrt", "IFrameworkView::Run (the game loop)");
        hr = call_slot<HRESULT>(view, S + 3);
        fl::log_write("winrt", "IFrameworkView::Run returned 0x%08x", static_cast<uint32_t>(hr));
        call_slot<HRESULT>(view, S + 4);  // Uninitialize
        com_release(view);
        return S_OK;
    }

    static Vtable& vt() {
        static Vtable v = Vtable("ICoreApplication", S + 9)
                              .set(S + 0, &get_Id)
                              .set(S + 1, &add_Suspending)
                              .set(S + 2, &remove_Suspending)
                              .set(S + 3, &add_Resuming)
                              .set(S + 4, &remove_Resuming)
                              // S+5 get_Properties: trap until needed
                              .set(S + 6, &GetCurrentView)
                              .set(S + 7, &Run);
        return v;
    }

    Iface iapp{vt(), this};
    Iface iresources{vt_resources(), this};
    CoreApplicationStatics() : Object(u"Windows.ApplicationModel.Core.CoreApplication") {
        expose(IID_ICoreApplication, &iapp);
        expose(IID_ICoreApplicationResourceAvailability, &iresources);
    }
};

HRESULT get_core_application(const GUID& iid, void** out) {
    static CoreApplicationStatics* s = [] {
        g_main_view = new CoreApplicationView();
        return new CoreApplicationStatics();
    }();
    return s->query(iid, out);
}

}  // namespace

WINRT_REGISTER_CLASS(u"Windows.ApplicationModel.Core.CoreApplication", get_core_application);

}  // namespace fl::winrt
