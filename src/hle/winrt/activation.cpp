#include "hle/winrt/activation.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <map>
#include <string>

#include "common/log.h"
#include "common/wstr.h"
#include "common/config.h"
#include "hle/winrt/com.h"
#include "hle/winrt/object.h"
#include "loader/modules.h"

namespace fl::winrt {
namespace {

std::map<std::u16string, FactoryGetter>& classes() {
    static std::map<std::u16string, FactoryGetter> m;
    return m;
}

// In-process servers declared by the package manifest (AppxManifest.xml):
// activatable class id -> DLL file name in the package.
const std::map<std::u16string, std::string>& manifest_classes() {
    static std::map<std::u16string, std::string> m = [] {
        std::map<std::u16string, std::string> out;
        std::ifstream f(config().game_dir + "/AppxManifest.xml");
        std::stringstream ss;
        ss << f.rdbuf();
        std::string xml = ss.str();
        size_t pos = 0;
        while ((pos = xml.find("<InProcessServer>", pos)) != std::string::npos) {
            size_t end = xml.find("</InProcessServer>", pos);
            size_t p0 = xml.find("<Path>", pos), p1 = xml.find("</Path>", pos);
            if (end == std::string::npos || p0 > end || p1 > end) break;
            std::string dll = xml.substr(p0 + 6, p1 - p0 - 6);
            for (size_t c = pos; (c = xml.find("ActivatableClassId=\"", c)) < end;) {
                c += 20;
                std::string id = xml.substr(c, xml.find('"', c) - c);
                out[utf8_to_utf16(id)] = dll;
            }
            pos = end;
        }
        return out;
    }();
    return m;
}

// Activate a class implemented by a package DLL through its own DllGetActivationFactory.
HRESULT activate_from_package(const char16_t* class_id, const std::string& dll, const GUID& iid, void** out) {
    loader::PeImage* img = loader::load_pe_module(config().game_dir + "/" + dll);
    if (!img) {
        FL_ERROR("package DLL %s (for %s) not found", dll.c_str(), utf16_to_utf8(class_id).c_str());
        return REGDB_E_CLASSNOTREG;
    }
    using GetFactory = HRESULT(WINAPI*)(HSTRING, void**);
    auto get = reinterpret_cast<GetFactory>(loader::pe_export(*img, "DllGetActivationFactory"));
    if (!get) fatal("%s has no DllGetActivationFactory", dll.c_str());
    HSTRING name = nullptr;
    make_hstring(class_id, &name);  // kept: one small string per class
    void* factory = nullptr;
    HRESULT hr = get(name, &factory);
    if (hr < 0) return hr;
    hr = call_slot<HRESULT>(factory, 0, &iid, out);  // QueryInterface
    com_release(factory);
    return hr;
}

}  // namespace

std::string guid_str(const GUID& g) {
    char buf[64];
    snprintf(buf, sizeof buf, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", g.Data1, g.Data2, g.Data3,
             g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}

ClassRegistrar::ClassRegistrar(const char16_t* name, FactoryGetter getter) { classes()[name] = getter; }

HRESULT get_activation_factory(const char16_t* class_id, const GUID* iid, void** out) {
    std::string name = utf16_to_utf8(class_id);
    auto it = classes().find(class_id);
    if (it == classes().end()) {
        auto pkg = manifest_classes().find(class_id);
        if (pkg != manifest_classes().end()) {
            HRESULT hr = activate_from_package(class_id, pkg->second, *iid, out);
            FL_LOG(winrt, "activation factory %s (package %s) iid %s -> 0x%08x", name.c_str(), pkg->second.c_str(),
                   guid_str(*iid).c_str(), static_cast<uint32_t>(hr));
            return hr;
        }
        FL_ERROR("UNIMPLEMENTED WinRT class %s (factory iid %s)", name.c_str(), guid_str(*iid).c_str());
        *out = nullptr;
        return REGDB_E_CLASSNOTREG;
    }
    HRESULT hr = it->second(*iid, out);
    FL_LOG(winrt, "activation factory %s iid %s -> 0x%08x", name.c_str(), guid_str(*iid).c_str(),
           static_cast<uint32_t>(hr));
    return hr;
}

}  // namespace fl::winrt
