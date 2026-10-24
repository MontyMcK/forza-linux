// Windows.ApplicationModel.Package: the app's identity, read at run time from
// the package's AppxManifest.xml (<Identity Name Publisher Version
// ProcessorArchitecture>) and the package folder name (publisher id suffix).
// Slot layouts: the proxy data (FINDINGS §4c) — same order as desktop.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "common/config.h"
#include "common/log.h"
#include "common/wstr.h"
#include "hle/winrt/activation.h"
#include "hle/winrt/object.h"

namespace fl::winrt {
namespace {

constexpr int S = kInspectableSlots;
constexpr GUID IID_IPackageStatics = {0x4E534BDF, 0x2960, 0x4878, {0x97, 0xA4, 0x96, 0x24, 0xDE, 0xB7, 0x2F, 0x2D}};
constexpr GUID IID_IPackage = {0x163C792F, 0xBD75, 0x413C, {0xBF, 0x23, 0xB1, 0xFE, 0x7B, 0x95, 0xD8, 0x25}};
constexpr GUID IID_IPackageId = {0x1ADB665E, 0x37C7, 0x4790, {0x99, 0x80, 0xDD, 0x7A, 0xE7, 0x4E, 0x8B, 0xB2}};

struct Identity {
    std::u16string name, publisher, publisher_id, full_name, family_name;
    uint16_t version[4] = {};
};

std::string attr(const std::string& xml, const std::string& elem_start, const char* key) {
    size_t e = xml.find(elem_start);
    if (e == std::string::npos) return {};
    size_t end = xml.find('>', e);
    std::string k = std::string(" ") + key + "=\"";
    size_t a = xml.find(k, e);
    if (a == std::string::npos || a > end) return {};
    a += k.size();
    return xml.substr(a, xml.find('"', a) - a);
}

const Identity& identity() {
    static Identity id = [] {
        Identity i;
        std::ifstream f(config().game_dir + "/AppxManifest.xml");
        std::stringstream ss;
        ss << f.rdbuf();
        std::string xml = ss.str();
        std::string name = attr(xml, "<Identity", "Name"), version = attr(xml, "<Identity", "Version");
        std::string arch = attr(xml, "<Identity", "ProcessorArchitecture");
        unsigned v[4] = {};
        std::sscanf(version.c_str(), "%u.%u.%u.%u", &v[0], &v[1], &v[2], &v[3]);
        for (int k = 0; k < 4; k++) i.version[k] = static_cast<uint16_t>(v[k]);
        // The package folder is named <Name>_<Version>_<arch>__<PublisherId>.
        std::string dir = config().game_dir;
        while (!dir.empty() && dir.back() == '/') dir.pop_back();
        if (dir.size() > 6 && dir.substr(dir.size() - 6) == "/Mount") dir.resize(dir.size() - 6);
        std::string folder = dir.substr(dir.find_last_of('/') + 1);
        std::string pub_id = folder.find("__") != std::string::npos ? folder.substr(folder.find("__") + 2) : "";
        i.name = utf8_to_utf16(name);
        i.publisher = utf8_to_utf16(attr(xml, "<Identity", "Publisher"));
        i.publisher_id = utf8_to_utf16(pub_id);
        i.full_name = utf8_to_utf16(name + "_" + version + "_" + arch + "__" + pub_id);
        i.family_name = utf8_to_utf16(name + "_" + pub_id);
        FL_LOG(winrt, "package identity %s", (name + "_" + version + "_" + arch + "__" + pub_id).c_str());
        return i;
    }();
    return id;
}

// ---- IPackageId (14 slots) ----
struct PackageId : Object {
    struct Version {
        uint16_t major, minor, build, revision;
    };
    static WINAPI HRESULT get_Name(Iface*, HSTRING* out) { return make_hstring(identity().name.c_str(), out); }
    static WINAPI HRESULT get_Version(Iface*, Version* out) {
        const uint16_t* v = identity().version;
        *out = {v[0], v[1], v[2], v[3]};
        return S_OK;
    }
    static WINAPI HRESULT get_Architecture(Iface*, int32_t* out) { return *out = 9 /* x64 */, S_OK; }
    static WINAPI HRESULT get_ResourceId(Iface*, HSTRING* out) { return *out = nullptr, S_OK; }
    static WINAPI HRESULT get_Publisher(Iface*, HSTRING* out) { return make_hstring(identity().publisher.c_str(), out); }
    static WINAPI HRESULT get_PublisherId(Iface*, HSTRING* out) {
        return make_hstring(identity().publisher_id.c_str(), out);
    }
    static WINAPI HRESULT get_FullName(Iface*, HSTRING* out) { return make_hstring(identity().full_name.c_str(), out); }
    static WINAPI HRESULT get_FamilyName(Iface*, HSTRING* out) {
        return make_hstring(identity().family_name.c_str(), out);
    }
    static Vtable& vt() {
        static Vtable v = Vtable("IPackageId", 14)
                              .set(S + 0, &get_Name)
                              .set(S + 1, &get_Version)
                              .set(S + 2, &get_Architecture)
                              .set(S + 3, &get_ResourceId)
                              .set(S + 4, &get_Publisher)
                              .set(S + 5, &get_PublisherId)
                              .set(S + 6, &get_FullName)
                              .set(S + 7, &get_FamilyName);
        return v;
    }
    Iface iid{vt(), this};
    PackageId() : Object(u"Windows.ApplicationModel.PackageId") { expose(IID_IPackageId, &iid); }
};

// ---- IPackage (9 slots): S+0 Id, S+1 InstalledLocation (StorageFolder: traps), S+2 IsFramework ----
struct Package : Object {
    static WINAPI HRESULT get_Id(Iface*, void** out) {
        static PackageId* id = new PackageId();
        return *out = id->default_iface(), S_OK;
    }
    static WINAPI HRESULT get_IsFramework(Iface*, uint8_t* out) { return *out = 0, S_OK; }
    static Vtable& vt() {
        static Vtable v = Vtable("IPackage", 9).set(S + 0, &get_Id).set(S + 2, &get_IsFramework);
        return v;
    }
    Iface ipkg{vt(), this};
    Package() : Object(u"Windows.ApplicationModel.Package") { expose(IID_IPackage, &ipkg); }
};

struct PackageStatics : Object {
    static WINAPI HRESULT get_Current(Iface*, void** out) {
        static Package* p = new Package();
        return *out = p->default_iface(), S_OK;
    }
    static Vtable& vt() {
        static Vtable v = Vtable("IPackageStatics", 7).set(S + 0, &get_Current);
        return v;
    }
    Iface istatics{vt(), this};
    PackageStatics() : Object(u"Windows.ApplicationModel.Package") { expose(IID_IPackageStatics, &istatics); }
};

HRESULT get_package_factory(const GUID& iid, void** out) {
    static auto* s = new PackageStatics();
    return s->query(iid, out);
}

}  // namespace

WINRT_REGISTER_CLASS(u"Windows.ApplicationModel.Package", get_package_factory);

}  // namespace fl::winrt
