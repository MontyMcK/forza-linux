// Windows.Xbox.Services.XboxLiveConfiguration: the title's Xbox Live identity.
// IXboxLiveConfigurationStatics (proxy data, FINDINGS §4c) has three HSTRING
// getters. Which is which is unverified: we assume S+0 sandbox, S+1 title id,
// S+2 primary service config id (the one the game reads, from three places).
// Values come from the package manifest's <mx:XboxLive TitleId
// PrimaryServiceConfigId> at run time.
#include <fstream>
#include <sstream>
#include <string>

#include "common/config.h"
#include "common/log.h"
#include "common/wstr.h"
#include "hle/winrt/activation.h"
#include "hle/winrt/object.h"

namespace fl::winrt {

std::u16string xbox_live_manifest_attr(const char* key) {
    std::ifstream f(config().game_dir + "/AppxManifest.xml");
    std::stringstream ss;
    ss << f.rdbuf();
    std::string xml = ss.str();
    size_t e = xml.find("<mx:XboxLive");
    if (e == std::string::npos) return {};
    std::string k = std::string(key) + "=\"";
    size_t a = xml.find(k, e);
    if (a == std::string::npos) return {};
    a += k.size();
    return utf8_to_utf16(xml.substr(a, xml.find('"', a) - a));
}

namespace {

constexpr int S = kInspectableSlots;
constexpr GUID IID_IXboxLiveConfigurationStatics = {0x00193376, 0x6A84, 0x4C67,
                                                    {0x84, 0xA5, 0x3B, 0x8A, 0x8A, 0xAE, 0xA7, 0x12}};

std::u16string manifest_attr(const char* key) { return xbox_live_manifest_attr(key); }

struct XboxLiveConfigurationStatics : Object {
    static WINAPI HRESULT get_SandboxId(Iface*, HSTRING* out) { return make_hstring(u"XDKS.1", out); }
    static WINAPI HRESULT get_TitleId(Iface*, HSTRING* out) {
        static std::u16string v = manifest_attr("TitleId");
        return make_hstring(v.c_str(), out);
    }
    static WINAPI HRESULT get_PrimaryServiceConfigId(Iface*, HSTRING* out) {
        static std::u16string v = manifest_attr("PrimaryServiceConfigId");
        return make_hstring(v.c_str(), out);
    }
    static Vtable& vt() {
        static Vtable v = Vtable("IXboxLiveConfigurationStatics", 9)
                              .set(S + 0, &get_SandboxId)
                              .set(S + 1, &get_TitleId)
                              .set(S + 2, &get_PrimaryServiceConfigId);
        return v;
    }
    Iface istatics{vt(), this};
    XboxLiveConfigurationStatics() : Object(u"Windows.Xbox.Services.XboxLiveConfiguration") {
        expose(IID_IXboxLiveConfigurationStatics, &istatics);
    }
};

HRESULT get_config(const GUID& iid, void** out) {
    static auto* s = new XboxLiveConfigurationStatics();
    return s->query(iid, out);
}

}  // namespace

WINRT_REGISTER_CLASS(u"Windows.Xbox.Services.XboxLiveConfiguration", get_config);

}  // namespace fl::winrt
