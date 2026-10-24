// Windows.Xbox.Input: one gamepad controller (SDL gamepad or keyboard, see
// platform/input.h), always connected and paired with the one user.
// Slot signatures from gameosproxy.dll's proxy data (FINDINGS §4c).
#include <mutex>
#include <vector>

#include "common/log.h"
#include "hle/winrt/activation.h"
#include "hle/winrt/collections.h"
#include "hle/winrt/object.h"
#include "hle/winrt/xbox.h"
#include "platform/input.h"

namespace fl::winrt {
namespace {

constexpr int S = kInspectableSlots;
constexpr GUID IID_IControllerStatics = {0x7400624F, 0x144B, 0x47FC, {0x9F, 0x3B, 0x1D, 0xCA, 0xFB, 0x48, 0x28, 0x73}};
constexpr GUID IID_IController = {0x9776FF56, 0x9BFD, 0x4581, {0xAD, 0x45, 0xB6, 0x45, 0xBB, 0xA5, 0x26, 0xD6}};
constexpr GUID IID_IGamepad = {0x082E402C, 0x07DF, 0x45E1, {0xA5, 0xAB, 0xA3, 0x12, 0x7A, 0xF1, 0x97, 0xB5}};
constexpr GUID IID_IGamepadReading = {0x4B5D17C9, 0xF757, 0x4F4B, {0x98, 0x16, 0x90, 0xBD, 0x1D, 0x64, 0xE7, 0x49}};
constexpr GUID IID_IVectorView_IController = {0xD2E6422E, 0xA5C0, 0x56E0,
                                              {0xB2, 0xEE, 0x55, 0x27, 0x39, 0xC2, 0xE0, 0xBC}};
constexpr uint64_t kControllerId = 1;

// Which reading API the game uses (each logged once, FL_LOG=input).
void log_once_input(const char* what) {
    static std::mutex m;
    static std::vector<const char*> seen;
    std::lock_guard lock(m);
    for (auto* s : seen)
        if (s == what) return;
    seen.push_back(what);
    FL_LOG(input, "game uses %s", what);
}

// RawGamepadReading (40 bytes per the proxy data: u64, enum32, 6 floats). Field
// order assumed to match the public Windows.Gaming.Input.GamepadReading.
struct RawReading {
    uint64_t timestamp;
    uint32_t buttons;
    float left_trigger, right_trigger, lx, ly, rx, ry;
    uint32_t pad;
};
static_assert(sizeof(RawReading) == 40);

RawReading raw_now() {
    platform::PadState p = platform::read_pad();
    return {p.timestamp, p.buttons, p.left_trigger, p.right_trigger, p.lx, p.ly, p.rx, p.ry, 0};
}

// ---- IGamepadReading (28 slots): S+0 timestamp, S+1 buttons, S+2..S+15 one
// bool per button, S+16..S+21 floats. The bool order is a guess (unverified);
// the floats follow the raw struct order.
struct GamepadReading : Object {
    RawReading r;
    static RawReading& rd(Iface* i) { return i->as<GamepadReading>()->r; }
    static WINAPI HRESULT get_Timestamp(Iface* i, uint64_t* out) { return *out = rd(i).timestamp, S_OK; }
    static WINAPI HRESULT get_Buttons(Iface* i, uint32_t* out) {
        log_once_input("IGamepadReading::get_Buttons");
        return *out = rd(i).buttons, S_OK;
    }
    template <uint32_t Bit>
    static WINAPI HRESULT get_Button(Iface* i, uint8_t* out) {
        log_once_input("IGamepadReading::Is*Pressed");
        return *out = (rd(i).buttons & Bit) != 0, S_OK;
    }
    template <int F>
    static WINAPI HRESULT get_Float(Iface* i, float* out) {
        const float* f = &rd(i).left_trigger;
        return *out = f[F], S_OK;
    }
    static Vtable& vt() {
        static Vtable v = Vtable("IGamepadReading", 28)
                              .set(S + 0, &get_Timestamp)
                              .set(S + 1, &get_Buttons)
                              .set(S + 2, &get_Button<0x40>)    // DPadUp
                              .set(S + 3, &get_Button<0x80>)    // DPadDown
                              .set(S + 4, &get_Button<0x100>)   // DPadLeft
                              .set(S + 5, &get_Button<0x200>)   // DPadRight
                              .set(S + 6, &get_Button<0x1>)     // Menu
                              .set(S + 7, &get_Button<0x2>)     // View
                              .set(S + 8, &get_Button<0x1000>)  // LeftThumbstick
                              .set(S + 9, &get_Button<0x2000>)  // RightThumbstick
                              .set(S + 10, &get_Button<0x400>)  // LeftShoulder
                              .set(S + 11, &get_Button<0x800>)  // RightShoulder
                              .set(S + 12, &get_Button<0x4>)    // A
                              .set(S + 13, &get_Button<0x8>)    // B
                              .set(S + 14, &get_Button<0x10>)   // X
                              .set(S + 15, &get_Button<0x20>)   // Y
                              .set(S + 16, &get_Float<0>)
                              .set(S + 17, &get_Float<1>)
                              .set(S + 18, &get_Float<2>)
                              .set(S + 19, &get_Float<3>)
                              .set(S + 20, &get_Float<4>)
                              .set(S + 21, &get_Float<5>);
        return v;
    }
    Iface ireading{vt(), this};
    explicit GamepadReading(const RawReading& raw) : Object(u"Windows.Xbox.Input.GamepadReading"), r(raw) {
        expose(IID_IGamepadReading, &ireading);
    }
};

// ---- the controller: IController (9 slots) + IGamepad (9 slots) ----
struct Controller : Object {
    static WINAPI HRESULT get_Id(Iface*, uint64_t* out) { return *out = kControllerId, S_OK; }
    static WINAPI HRESULT get_Type(Iface*, HSTRING* out) { return make_hstring(u"Windows.Xbox.Input.Gamepad", out); }
    static WINAPI HRESULT get_User(Iface*, void** out) { return *out = xbox_user(), S_OK; }
    // IGamepad S+0: SetVibration(GamepadVibration {left, right, left trigger, right trigger} by value, 16 bytes).
    struct Vibration {
        float left, right, left_trigger, right_trigger;
    };
    static WINAPI HRESULT SetVibration(Iface*, Vibration v) {
        platform::set_vibration(v.left, v.right, v.left_trigger, v.right_trigger);
        return S_OK;
    }
    static WINAPI HRESULT GetCurrentReading(Iface*, void** out) {
        log_once_input("IGamepad::GetCurrentReading");
        *out = (new GamepadReading(raw_now()))->default_iface_noref();
        return S_OK;
    }
    static WINAPI HRESULT GetRawCurrentReading(Iface*, RawReading* out) {
        log_once_input("IGamepad::GetRawCurrentReading");
        return *out = raw_now(), S_OK;
    }

    static Vtable& controller_vt() {
        static Vtable v = Vtable("IController", 9).set(S + 0, &get_Id).set(S + 1, &get_Type).set(S + 2, &get_User);
        return v;
    }
    static Vtable& gamepad_vt() {
        static Vtable v = Vtable("IGamepad", 9)
                              .set(S + 0, &SetVibration)
                              .set(S + 1, &GetCurrentReading)
                              .set(S + 2, &GetRawCurrentReading);
        return v;
    }
    Iface icontroller{controller_vt(), this};
    Iface igamepad{gamepad_vt(), this};
    Controller() : Object(u"Windows.Xbox.Input.Gamepad") {
        expose(IID_IController, &icontroller);
        expose(IID_IGamepad, &igamepad);
    }
};

Controller* the_controller() {
    static Controller* c = new Controller();
    return c;
}

// ---- IControllerStatics (18 slots) ----
enum { kAdded, kRemoved, kPairingChanged, kOrderChanged, kEvents };

struct ControllerStatics : Object {
    EventSource events[kEvents];
    static WINAPI HRESULT get_Controllers(Iface*, void** out) {
        *out = (new VectorView(IID_IVectorView_IController, {xbox_controller()}))->default_iface_noref();
        return S_OK;
    }
    template <int E>
    static WINAPI HRESULT add(Iface* i, void* h, EventToken* t) {
        return i->as<ControllerStatics>()->events[E].add(h, t);
    }
    template <int E>
    static WINAPI HRESULT remove(Iface* i, EventToken t) {
        return i->as<ControllerStatics>()->events[E].remove(t);
    }
    static WINAPI HRESULT GetControllerById(Iface*, uint64_t id, void** out) {
        if (id != kControllerId) return *out = nullptr, E_INVALIDARG;
        return *out = xbox_controller(), S_OK;
    }
    static Vtable& vt() {
        static Vtable v = Vtable("IControllerStatics", 18)
                              .set(S + 0, &get_Controllers)
                              .set(S + 1, &add<kAdded>)
                              .set(S + 2, &remove<kAdded>)
                              .set(S + 3, &add<kRemoved>)
                              .set(S + 4, &remove<kRemoved>)
                              .set(S + 5, &add<kPairingChanged>)
                              .set(S + 6, &remove<kPairingChanged>)
                              .set(S + 7, &add<kOrderChanged>)
                              .set(S + 8, &remove<kOrderChanged>)
                              .set(S + 9, &GetControllerById);
        return v;
    }
    Iface istatics{vt(), this};
    ControllerStatics() : Object(u"Windows.Xbox.Input.Controller") { expose(IID_IControllerStatics, &istatics); }
};

HRESULT get_controller_factory(const GUID& iid, void** out) {
    static auto* s = new ControllerStatics();
    return s->query(iid, out);
}

}  // namespace

void* xbox_controller() { return the_controller()->default_iface(); }

WINRT_REGISTER_CLASS(u"Windows.Xbox.Input.Controller", get_controller_factory);

}  // namespace fl::winrt
