// Windows.Xbox.System: one signed-in local user ("Player").
//
// Slot signatures come from the MIDL proxy data in EmbeddedXvd's
// gameosproxy.dll (tools/midl_proxies.py, FINDINGS §4c): proc number = slot,
// parameter directions/types are exact, method names are inferred from the
// types (event delegate types name the events). S = 6 IInspectable slots.
#include "common/log.h"
#include "common/wstr.h"
#include "hle/winrt/activation.h"
#include "hle/winrt/async.h"
#include "hle/winrt/collections.h"
#include "hle/winrt/object.h"
#include "hle/winrt/xbox.h"

namespace fl::winrt {
namespace {

constexpr int S = kInspectableSlots;

constexpr GUID IID_IUserStatics = {0xB4F23A93, 0x7FC6, 0x4A50, {0xB1, 0x53, 0x95, 0x00, 0x16, 0x38, 0x9E, 0x50}};
constexpr GUID IID_IUser = {0xA2C2223E, 0x2F00, 0x44E2, {0x89, 0x7E, 0x01, 0x5B, 0xCE, 0xB8, 0x77, 0x17}};
constexpr GUID IID_IAsyncOperation_GetTokenAndSignatureResult = {
    0xE6C6E9B1, 0x363D, 0x54FE, {0x80, 0x8B, 0x89, 0xDA, 0x6B, 0xCF, 0x78, 0xBF}};
// HRESULT_FROM_WIN32(ERROR_NETWORK_UNREACHABLE): the console is offline (see networking.cpp).
constexpr HRESULT kNetworkUnreachable = static_cast<HRESULT>(0x800704CF);
constexpr GUID IID_IUserDisplayInfo = {0xB86DC0DC, 0xCAEF, 0x4A9B, {0x91, 0xC0, 0x2D, 0x1B, 0x67, 0xFC, 0xCD, 0xF1}};
// IVectorView<User> / IVectorView<IController> / IVectorView<IAudioDeviceInfo> (pinterface IIDs from the proxy data)
constexpr GUID IID_IVectorView_User = {0x39CF85CC, 0xA83F, 0x5BCD, {0x9B, 0x68, 0xD4, 0xD5, 0xCC, 0xCB, 0x6A, 0x9F}};
constexpr GUID IID_IVectorView_IController = {0xD2E6422E, 0xA5C0, 0x56E0,
                                              {0xB2, 0xEE, 0x55, 0x27, 0x39, 0xC2, 0xE0, 0xBC}};
constexpr GUID IID_IVectorView_IAudioDeviceInfo = {0x2DBCE3C8, 0x0D1B, 0x5181,
                                                   {0xAF, 0x07, 0x9C, 0x35, 0xAD, 0x0C, 0x55, 0x6E}};

constexpr uint32_t kUserId = 1;
constexpr const char16_t* kGamertag = u"Player";
constexpr const char16_t* kXuid = u"2814612345678901";  // made up, but shaped like a real XUID

// ---- IUserDisplayInfo (15 slots) ----
struct UserDisplayInfo : Object {
    // S+0 / S+2 / S+3: strings. Which is the gamertag vs. display names is
    // unknown; all of them say "Player".
    static WINAPI HRESULT get_String(Iface*, HSTRING* out) { return make_hstring(kGamertag, out); }
    // S+1 / S+4: u32s (gamerscore? reputation?). 0 is a valid value for both.
    static WINAPI HRESULT get_Zero(Iface*, uint32_t* out) {
        *out = 0;
        return S_OK;
    }
    // STUB: S+5 is an enum the game stores as a byte (rva 0x124b7a6). Next to
    // the names it is most likely the age group (0 unknown, 1 child, 2 teen,
    // 3 adult); adult is the least restrictive answer.
    static WINAPI HRESULT get_AgeGroup(Iface*, uint32_t* out) {
        FL_STUB_ONCE("UserDisplayInfo slot S+5 (age group?) returns 3");
        *out = 3;
        return S_OK;
    }
    // S+6: a vector view with IID {E5CE1A07-...} (probably IVectorView<uint32> privileges). Empty.
    static WINAPI HRESULT get_Privileges(Iface*, void** out) {
        constexpr GUID iid = {0xE5CE1A07, 0x8D33, 0x5007, {0xBA, 0x64, 0x7D, 0x25, 0x08, 0xCC, 0xF8, 0x5C}};
        *out = (new VectorView(iid, {}))->default_iface_noref();
        return S_OK;
    }
    static Vtable& vt() {
        static Vtable v = Vtable("IUserDisplayInfo", 15)
                              .set(S + 5, &get_AgeGroup)
                              .set(S + 6, &get_Privileges)
                              .set(S + 0, &get_String)
                              .set(S + 1, &get_Zero)
                              .set(S + 2, &get_String)
                              .set(S + 3, &get_String)
                              .set(S + 4, &get_Zero);
        return v;
    }
    Iface iinfo{vt(), this};
    UserDisplayInfo() : Object(u"Windows.Xbox.System.UserDisplayInfo") { expose(IID_IUserDisplayInfo, &iinfo); }
};

// ---- IUser (19 slots) ----
struct User : Object {
    static User* self(Iface* i) { return i->as<User>(); }
    static WINAPI HRESULT get_Id(Iface*, uint32_t* out) {
        *out = kUserId;
        return S_OK;
    }
    static WINAPI HRESULT get_AudioDevices(Iface*, void** out) {
        *out = (new VectorView(IID_IVectorView_IAudioDeviceInfo, {}))->default_iface_noref();
        return S_OK;
    }
    // The one gamepad is paired with the one user.
    static WINAPI HRESULT get_Controllers(Iface*, void** out) {
        *out = (new VectorView(IID_IVectorView_IController, {xbox_controller()}))->default_iface_noref();
        return S_OK;
    }
    static WINAPI HRESULT get_DisplayInfo(Iface* i, void** out) {
        *out = self(i)->display->default_iface();
        return S_OK;
    }
    static WINAPI HRESULT get_IsGuest(Iface*, uint8_t* out) {
        *out = 0;
        return S_OK;
    }
    static WINAPI HRESULT get_IsSignedIn(Iface*, uint8_t* out) {
        *out = 1;
        return S_OK;
    }
    static WINAPI HRESULT get_Sponsor(Iface*, void** out) {  // guests only
        *out = nullptr;
        return S_OK;
    }
    static WINAPI HRESULT get_XboxUserHash(Iface*, HSTRING* out) { return make_hstring(u"0", out); }
    static WINAPI HRESULT get_XboxUserId(Iface*, HSTRING* out) { return make_hstring(kXuid, out); }
    // S+10..S+12 GetTokenAndSignatureAsync(method, url, headers[, body]): Xbox
    // Live auth for HTTP requests. Offline: the operation fails like a request
    // with no network would, and Xbox Services reports the call as failed.
    static WINAPI HRESULT GetTokenAndSignature(Iface*, HSTRING method, HSTRING url, HSTRING, void** out) {
        FL_LOG(winrt, "User::GetTokenAndSignatureAsync(%s %s) -> offline", utf16_to_utf8(hstring_str(method)).c_str(),
               utf16_to_utf8(hstring_str(url)).c_str());
        *out = async_failed(IID_IAsyncOperation_GetTokenAndSignatureResult, kNetworkUnreachable);
        return S_OK;
    }
    static WINAPI HRESULT GetTokenAndSignatureBody(Iface* i, HSTRING m, HSTRING u, HSTRING h, uint32_t, const uint8_t*,
                                                   void** out) {
        return GetTokenAndSignature(i, m, u, h, out);
    }
    static WINAPI HRESULT GetTokenAndSignatureStr(Iface* i, HSTRING m, HSTRING u, HSTRING h, HSTRING, void** out) {
        return GetTokenAndSignature(i, m, u, h, out);
    }
    static Vtable& vt() {
        static Vtable v = Vtable("IUser", 19)
                              .set(S + 0, &get_Id)
                              .set(S + 1, &get_AudioDevices)
                              .set(S + 2, &get_Controllers)
                              .set(S + 3, &get_DisplayInfo)
                              .set(S + 4, &get_IsGuest)
                              .set(S + 5, &get_IsSignedIn)
                              .set(S + 7, &get_Sponsor)
                              .set(S + 8, &get_XboxUserHash)
                              .set(S + 9, &get_XboxUserId)
                              .set(S + 10, &GetTokenAndSignature)
                              .set(S + 11, &GetTokenAndSignatureBody)
                              .set(S + 12, &GetTokenAndSignatureStr);
        return v;
    }
    Iface iuser{vt(), this};
    UserDisplayInfo* display = new UserDisplayInfo();
    User() : Object(u"Windows.Xbox.System.User") { expose(IID_IUser, &iuser); }
};

User* the_user() {
    static User* u = new User();
    return u;
}

// ---- IUserStatics (33 slots) ----
// Events (delegate types from the proxy data). None of them ever fire: the
// one user stays signed in.
enum StaticEvent { kOnlineStateChanged, kUserAdded, kUserRemoved, kAudioAdded, kAudioRemoved, kAudioChanged,
                   kSignInCompleted, kSignOutStarted, kSignOutCompleted, kDisplayInfoChanged, kEventCount };

struct UserStatics : Object {
    EventSource events[kEventCount];
    static UserStatics* self(Iface* i) { return i->as<UserStatics>(); }

    template <int E>
    static WINAPI HRESULT add(Iface* i, void* handler, EventToken* tok) {
        return self(i)->events[E].add(handler, tok);
    }
    template <int E>
    static WINAPI HRESULT remove(Iface* i, EventToken tok) {
        return self(i)->events[E].remove(tok);
    }
    static WINAPI HRESULT get_Users(Iface*, void** out) {
        FL_LOG(winrt, "User::Users -> 1 signed-in user");
        *out = (new VectorView(IID_IVectorView_User, {the_user()->default_iface()}))->default_iface_noref();
        return S_OK;
    }
    static WINAPI HRESULT GetUserById(Iface*, uint32_t id, void** out) {
        if (id != kUserId) {
            *out = nullptr;
            return E_INVALIDARG;
        }
        *out = the_user()->default_iface();
        return S_OK;
    }
    static Vtable& vt() {
        static Vtable v = Vtable("IUserStatics", 33)
                              .set(S + 1, &add<kOnlineStateChanged>)
                              .set(S + 2, &remove<kOnlineStateChanged>)
                              .set(S + 3, &get_Users)
                              .set(S + 4, &add<kUserAdded>)
                              .set(S + 5, &remove<kUserAdded>)
                              .set(S + 6, &add<kUserRemoved>)
                              .set(S + 7, &remove<kUserRemoved>)
                              .set(S + 8, &GetUserById)
                              .set(S + 12, &add<kAudioAdded>)
                              .set(S + 13, &remove<kAudioAdded>)
                              .set(S + 14, &add<kAudioRemoved>)
                              .set(S + 15, &remove<kAudioRemoved>)
                              .set(S + 16, &add<kAudioChanged>)
                              .set(S + 17, &remove<kAudioChanged>)
                              .set(S + 18, &add<kSignInCompleted>)
                              .set(S + 19, &remove<kSignInCompleted>)
                              .set(S + 20, &add<kSignOutStarted>)
                              .set(S + 21, &remove<kSignOutStarted>)
                              .set(S + 22, &add<kSignOutCompleted>)
                              .set(S + 23, &remove<kSignOutCompleted>)
                              .set(S + 24, &add<kDisplayInfoChanged>)
                              .set(S + 25, &remove<kDisplayInfoChanged>);
        return v;
    }
    Iface istatics{vt(), this};
    UserStatics() : Object(u"Windows.Xbox.System.User") { expose(IID_IUserStatics, &istatics); }
};

HRESULT get_user_factory(const GUID& iid, void** out) {
    static auto* s = new UserStatics();
    return s->query(iid, out);
}

}  // namespace

void* xbox_user() { return the_user()->default_iface(); }

WINRT_REGISTER_CLASS(u"Windows.Xbox.System.User", get_user_factory);

}  // namespace fl::winrt
