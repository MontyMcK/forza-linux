// Windows.Xbox.Media.GameTransportControls (media buttons/now-playing info for
// the system UI). IGameTransportControls (29 slots, proxy data FINDINGS §4c) is
// a property bag: S+0..S+3 two string get/put pairs, S+4/S+5 an enum pair,
// S+6 a read-only enum, S+7..S+18 six bool get/put pairs, then ButtonPressed and
// PropertyChanged add/remove. We store what the game sets; events never fire.
#include <string>

#include "common/log.h"
#include "hle/winrt/activation.h"
#include "hle/winrt/object.h"

namespace fl::winrt {
namespace {

constexpr int S = kInspectableSlots;
constexpr GUID IID_IGameTransportControls = {0xF5BA60D7, 0x9303, 0x44CD,
                                             {0xAC, 0x0C, 0x4E, 0x53, 0x27, 0x02, 0xCD, 0x00}};

struct GameTransportControls : Object {
    std::u16string str[2];
    int32_t enums[2] = {};
    uint8_t bools[6] = {};
    EventSource events[2];
    static GameTransportControls* self(Iface* i) { return i->as<GameTransportControls>(); }

    template <int N>
    static WINAPI HRESULT get_str(Iface* i, HSTRING* out) { return make_hstring(self(i)->str[N].c_str(), out); }
    template <int N>
    static WINAPI HRESULT put_str(Iface* i, HSTRING v) { return self(i)->str[N] = hstring_str(v), S_OK; }
    template <int N>
    static WINAPI HRESULT get_enum(Iface* i, int32_t* out) { return *out = self(i)->enums[N], S_OK; }
    template <int N>
    static WINAPI HRESULT put_enum(Iface* i, int32_t v) { return self(i)->enums[N] = v, S_OK; }
    template <int N>
    static WINAPI HRESULT get_bool(Iface* i, uint8_t* out) { return *out = self(i)->bools[N], S_OK; }
    template <int N>
    static WINAPI HRESULT put_bool(Iface* i, uint8_t v) { return self(i)->bools[N] = v, S_OK; }
    template <int E>
    static WINAPI HRESULT add(Iface* i, void* h, EventToken* t) { return self(i)->events[E].add(h, t); }
    template <int E>
    static WINAPI HRESULT remove(Iface* i, EventToken t) { return self(i)->events[E].remove(t); }

    static Vtable& vt() {
        static Vtable v = Vtable("IGameTransportControls", 29)
                              .set(S + 0, &get_str<0>).set(S + 1, &put_str<0>)
                              .set(S + 2, &get_str<1>).set(S + 3, &put_str<1>)
                              .set(S + 4, &get_enum<0>).set(S + 5, &put_enum<0>)
                              .set(S + 6, &get_enum<1>)
                              .set(S + 7, &get_bool<0>).set(S + 8, &put_bool<0>)
                              .set(S + 9, &get_bool<1>).set(S + 10, &put_bool<1>)
                              .set(S + 11, &get_bool<2>).set(S + 12, &put_bool<2>)
                              .set(S + 13, &get_bool<3>).set(S + 14, &put_bool<3>)
                              .set(S + 15, &get_bool<4>).set(S + 16, &put_bool<4>)
                              .set(S + 17, &get_bool<5>).set(S + 18, &put_bool<5>)
                              .set(S + 19, &add<0>).set(S + 20, &remove<0>)
                              .set(S + 21, &add<1>).set(S + 22, &remove<1>);
        return v;
    }
    Iface ictl{vt(), this};
    GameTransportControls() : Object(u"Windows.Xbox.Media.GameTransportControls") {
        expose(IID_IGameTransportControls, &ictl);
    }
};

HRESULT get_gtc(const GUID& iid, void** out) {
    static auto* f = new ActivationFactory(u"Windows.Xbox.Media.GameTransportControls",
                                           [] { return (new GameTransportControls())->default_iface_noref(); });
    return f->query(iid, out);
}

}  // namespace

WINRT_REGISTER_CLASS(u"Windows.Xbox.Media.GameTransportControls", get_gtc);

}  // namespace fl::winrt
