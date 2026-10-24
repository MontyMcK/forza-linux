// Windows.Networking.Connectivity.NetworkInformation, offline: no connection
// profiles, no internet profile, the status-changed event never fires.
// INetworkInformationStatics layout from the proxy data (same as desktop).
#include "common/log.h"
#include "hle/winrt/activation.h"
#include "hle/winrt/collections.h"
#include "hle/winrt/object.h"

namespace fl::winrt {
namespace {

constexpr int S = kInspectableSlots;
constexpr GUID IID_INetworkInformationStatics = {0x5074F851, 0x950D, 0x4165,
                                                 {0x9C, 0x15, 0x36, 0x56, 0x19, 0x48, 0x1E, 0xEA}};
constexpr GUID IID_IVectorView_ConnectionProfile = {0x6DB1B492, 0x3852, 0x5DF8,
                                                    {0xA2, 0x9D, 0x69, 0x44, 0x00, 0x2F, 0x58, 0xD4}};
constexpr GUID IID_IVectorView_LanIdentifier = {0x41286159, 0xB91D, 0x5736,
                                                {0xAD, 0x8B, 0xE1, 0x6F, 0xCF, 0x8A, 0xCE, 0xD0}};
constexpr GUID IID_IVectorView_HostName = {0xF4706AB1, 0x55A3, 0x5270, {0xAF, 0xB2, 0x73, 0x29, 0x88, 0xFE, 0x82, 0x27}};

struct NetworkInformationStatics : Object {
    EventSource status_changed;
    static WINAPI HRESULT GetConnectionProfiles(Iface*, void** out) {
        return *out = (new VectorView(IID_IVectorView_ConnectionProfile, {}))->default_iface_noref(), S_OK;
    }
    static WINAPI HRESULT GetInternetConnectionProfile(Iface*, void** out) {
        FL_LOG(winrt, "NetworkInformation::GetInternetConnectionProfile -> none (offline)");
        return *out = nullptr, S_OK;
    }
    static WINAPI HRESULT GetLanIdentifiers(Iface*, void** out) {
        return *out = (new VectorView(IID_IVectorView_LanIdentifier, {}))->default_iface_noref(), S_OK;
    }
    static WINAPI HRESULT GetHostNames(Iface*, void** out) {
        return *out = (new VectorView(IID_IVectorView_HostName, {}))->default_iface_noref(), S_OK;
    }
    static WINAPI HRESULT add_NetworkStatusChanged(Iface* i, void* h, EventToken* t) {
        return i->as<NetworkInformationStatics>()->status_changed.add(h, t);
    }
    static WINAPI HRESULT remove_NetworkStatusChanged(Iface* i, EventToken t) {
        return i->as<NetworkInformationStatics>()->status_changed.remove(t);
    }
    static Vtable& vt() {
        static Vtable v = Vtable("INetworkInformationStatics", 14)
                              .set(S + 0, &GetConnectionProfiles)
                              .set(S + 1, &GetInternetConnectionProfile)
                              .set(S + 2, &GetLanIdentifiers)
                              .set(S + 3, &GetHostNames)
                              .set(S + 6, &add_NetworkStatusChanged)
                              .set(S + 7, &remove_NetworkStatusChanged);
        return v;
    }
    Iface istatics{vt(), this};
    NetworkInformationStatics() : Object(u"Windows.Networking.Connectivity.NetworkInformation") {
        expose(IID_INetworkInformationStatics, &istatics);
    }
};

HRESULT get_netinfo(const GUID& iid, void** out) {
    static auto* s = new NetworkInformationStatics();
    return s->query(iid, out);
}

}  // namespace

WINRT_REGISTER_CLASS(u"Windows.Networking.Connectivity.NetworkInformation", get_netinfo);

}  // namespace fl::winrt
