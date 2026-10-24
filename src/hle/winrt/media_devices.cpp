// Windows.Media.Devices.MediaDevice statics. FMOD's WASAPI output asks for the
// default render device id (game rva 0x1a62690: IMediaDeviceStatics slot S+4)
// and hands it to ActivateAudioInterfaceAsync, which ignores it.
// Slot order = desktop windows.media.devices.idl.
#include "common/log.h"
#include "hle/winrt/activation.h"
#include "hle/winrt/object.h"

namespace fl::winrt {
namespace {

constexpr int S = kInspectableSlots;
constexpr GUID IID_IMediaDeviceStatics = {0xAA2D9A40, 0x909F, 0x4BBA, {0xBF, 0x8B, 0x0C, 0x0D, 0x29, 0x6F, 0x14, 0xF0}};

constexpr char16_t kRenderId[] = u"{0.0.0.00000000}.{forza-linux-default-render}";

struct MediaDeviceStatics : Object {
    static WINAPI HRESULT GetAudioRenderSelector(Iface*, HSTRING* out) { return make_hstring(u"", out); }
    // No capture devices (no Kinect / headset microphone).
    static WINAPI HRESULT GetDefaultAudioCaptureId(Iface*, int32_t role, HSTRING* out) { return *out = nullptr, S_OK; }
    static WINAPI HRESULT GetDefaultAudioRenderId(Iface*, int32_t role, HSTRING* out) {
        FL_LOG(winrt, "MediaDevice::GetDefaultAudioRenderId(role %d)", role);
        return make_hstring(kRenderId, out);
    }
    // STUB: the default device never changes, so handlers are never invoked.
    static WINAPI HRESULT add_Changed(Iface*, void* handler, EventToken* token) {
        FL_STUB_ONCE("MediaDevice default-device-changed events never fire");
        token->value = 1;
        return S_OK;
    }
    static WINAPI HRESULT remove_Changed(Iface*, EventToken) { return S_OK; }
    static Vtable& vt() {
        static Vtable v = Vtable("IMediaDeviceStatics", S + 9)
                              .set(S + 1, &GetAudioRenderSelector)
                              .set(S + 3, &GetDefaultAudioCaptureId)
                              .set(S + 4, &GetDefaultAudioRenderId)
                              .set(S + 5, &add_Changed)
                              .set(S + 6, &remove_Changed)
                              .set(S + 7, &add_Changed)
                              .set(S + 8, &remove_Changed);
        return v;
    }
    Iface istatics{vt(), this};
    MediaDeviceStatics() : Object(u"Windows.Media.Devices.MediaDevice") { expose(IID_IMediaDeviceStatics, &istatics); }
};

HRESULT get_media_device_factory(const GUID& iid, void** out) {
    static auto* s = new MediaDeviceStatics();
    return s->query(iid, out);
}

}  // namespace

WINRT_REGISTER_CLASS(u"Windows.Media.Devices.MediaDevice", get_media_device_factory);

}  // namespace fl::winrt
