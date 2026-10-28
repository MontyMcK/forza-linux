// Shared device-child plumbing: private data, the DXVK<->wrapper registry and
// the ID3D11DeviceChild base slots.
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <set>
#include <unordered_map>

#include "common/log.h"
#include "common/wstr.h"
#include "hle/d3d11x/d3d11x.h"
#include "hle/d3d11x/dxvk.h"

namespace fl::d3d {
namespace {

using winrt::Iface;

std::string key(const GUID& g) { return std::string(reinterpret_cast<const char*>(&g), sizeof(GUID)); }

// WKPDID_D3DDebugObjectName {429B8C22-9188-4B0C-8742-ACB0BF85C200}
constexpr GUID kDebugObjectName = {0x429B8C22, 0x9188, 0x4B0C, {0x87, 0x42, 0xAC, 0xB0, 0xBF, 0x85, 0xC2, 0x00}};

std::mutex g_reg_mutex;
std::unordered_map<void*, DeviceChild*> g_registry;  // DXVK object -> wrapper

WINAPI void child_GetDevice(Iface* self, void** out) {
    auto* d = self->as<DeviceChild>()->device;
    d->add_ref();
    *out = &d->device_iface;
}
WINAPI HRESULT child_GetPrivateData(Iface* self, const GUID& g, UINT* size, void* data) {
    return self->as<DeviceChild>()->priv.get(g, size, data);
}
WINAPI HRESULT child_SetPrivateData(Iface* self, const GUID& g, UINT size, const void* data) {
    return self->as<DeviceChild>()->priv.set(g, size, data);
}
WINAPI HRESULT child_SetPrivateDataInterface(Iface* self, const GUID& g, void* unknown) {
    return self->as<DeviceChild>()->priv.set_interface(g, unknown);
}

}  // namespace

void log_once(const char* what, uint64_t value) {
    static std::mutex m;
    static std::set<std::pair<std::string, uint64_t>> seen;
    std::lock_guard lock(m);
    if (seen.insert({what, value}).second) log_write("d3d", "%s 0x%llx", what, static_cast<unsigned long long>(value));
}

WINAPI HRESULT d3d_QueryInterface(Iface* self, const GUID& iid, void** out) {
    HRESULT hr = self->owner->query(iid, out);
    if (hr < 0)
        FL_ERROR("d3d: %s::QueryInterface(%s) -> E_NOINTERFACE", utf16_to_utf8(self->owner->class_name).c_str(),
                 winrt::guid_str(iid).c_str());
    return hr;
}

PrivateData::~PrivateData() {
    for (auto& [k, e] : entries_)
        if (e.iface) winrt::com_release(e.iface);
}

HRESULT PrivateData::get(const GUID& g, UINT* size, void* data) {
    if (!size) return E_INVALIDARG;
    std::lock_guard lock(m_);
    auto it = entries_.find(key(g));
    if (it == entries_.end()) {
        *size = 0;
        return DXGI_ERROR_NOT_FOUND;
    }
    const Entry& e = it->second;
    UINT need = e.iface ? sizeof(void*) : static_cast<UINT>(e.bytes.size());
    if (!data) {
        *size = need;
        return S_OK;
    }
    if (*size < need) {
        *size = need;
        return DXGI_ERROR_MORE_DATA;
    }
    *size = need;
    if (e.iface) {
        winrt::com_add_ref(e.iface);
        std::memcpy(data, &e.iface, sizeof(void*));
    } else {
        std::memcpy(data, e.bytes.data(), need);
    }
    return S_OK;
}

HRESULT PrivateData::set(const GUID& g, UINT size, const void* data) {
    void* old_iface = nullptr;
    {
        std::lock_guard lock(m_);
        auto it = entries_.find(key(g));
        if (it != entries_.end()) {
            old_iface = it->second.iface;
            entries_.erase(it);
        }
        if (data && size) {
            Entry& e = entries_[key(g)];
            e.bytes.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
        }
    }
    if (old_iface) winrt::com_release(old_iface);
    return S_OK;
}

HRESULT PrivateData::set_interface(const GUID& g, void* unknown) {
    set(g, 0, nullptr);
    if (!unknown) return S_OK;
    winrt::com_add_ref(unknown);
    std::lock_guard lock(m_);
    entries_[key(g)].iface = unknown;
    return S_OK;
}

std::string PrivateData::name() {
    std::lock_guard lock(m_);
    auto it = entries_.find(key(kDebugObjectName));
    if (it == entries_.end()) return {};
    const auto& b = it->second.bytes;
    return std::string(b.begin(), b.end()).c_str();
}

DeviceChild::DeviceChild(const char16_t* name, DeviceX* dev, winrt::Vtable& vt, void* dxvk_obj)
    : Object(name), device(dev), dxvk(dxvk_obj), iface(vt, this) {
    device->add_ref();
    expose(IID_ID3D11DeviceChild, &iface);
    if (dxvk) {
        std::lock_guard lock(g_reg_mutex);
        g_registry[dxvk] = this;
    }
}

DeviceChild::~DeviceChild() {
    magic = 0;
    if (dxvk) {
        {
            std::lock_guard lock(g_reg_mutex);
            auto it = g_registry.find(dxvk);
            if (it != g_registry.end() && it->second == this) g_registry.erase(it);
        }
        dx_release(dxvk);
    }
    device->release();
}

// FL_D3D_QUARANTINE=1: a wrapper released to zero is kept (with its DXVK
// object) instead of freed, so a later use of it by the game fails loudly in
// child_of() with a backtrace instead of corrupting DXVK. Diagnostic only.
void DeviceChild::destroy() {
    static const bool quarantine = getenv("FL_D3D_QUARANTINE") != nullptr;
    if (!quarantine) {
        delete this;
        return;
    }
    magic = kDeadMagic;
}

void register_alias(void* dxvk_obj, DeviceChild* w) {
    std::lock_guard lock(g_reg_mutex);
    g_registry[dxvk_obj] = w;
}

void unregister_alias(void* dxvk_obj, DeviceChild* w) {
    std::lock_guard lock(g_reg_mutex);
    auto it = g_registry.find(dxvk_obj);
    if (it != g_registry.end() && it->second == w) g_registry.erase(it);
}

DeviceChild* wrapper_of(void* dxvk_obj) {
    if (!dxvk_obj) return nullptr;
    std::lock_guard lock(g_reg_mutex);
    auto it = g_registry.find(dxvk_obj);
    return it == g_registry.end() ? nullptr : it->second;
}

DeviceChild* child_of(const void* game_iface) {
    if (!game_iface) return nullptr;
    auto* iface = static_cast<const Iface*>(game_iface);
    auto* c = static_cast<DeviceChild*>(iface->owner);
    if (c && c->magic == DeviceChild::kDeadMagic)
        fatal("d3d: the game used %p (%s), a D3D object it had released", game_iface,
              utf16_to_utf8(c->class_name).c_str());
    if (!c || c->magic != DeviceChild::kMagic)
        fatal("d3d: the game passed %p where a D3D object of ours was expected", game_iface);
    return c;
}

void* unwrap(const void* game_iface) { return game_iface ? child_of(game_iface)->dxvk : nullptr; }

winrt::Vtable& child_base(winrt::Vtable& vt) {
    return vt.set(0, &d3d_QueryInterface)
        .set(3, &child_GetDevice)
        .set(4, &child_GetPrivateData)
        .set(5, &child_SetPrivateData)
        .set(6, &child_SetPrivateDataInterface);
}

Resource::Resource(const char16_t* name, DeviceX* dev, winrt::Vtable& vt, void* dxvk_obj, int dimension,
                   const void* desc, size_t desc_size)
    : DeviceChild(name, dev, vt, dxvk_obj), dim(dimension), xdesc_size(desc_size) {
    std::memcpy(xdesc, desc, desc_size);
    expose(IID_ID3D11Resource, &iface);
}

namespace {
std::mutex g_hash_mutex;
std::unordered_map<void*, uint64_t> g_shader_hash;
}  // namespace
void note_shader_hash(void* dxvk_shader, uint64_t hash) {
    std::lock_guard lock(g_hash_mutex);
    g_shader_hash[dxvk_shader] = hash;
}
uint64_t shader_hash(void* dxvk_shader) {
    std::lock_guard lock(g_hash_mutex);
    auto it = g_shader_hash.find(dxvk_shader);
    return it == g_shader_hash.end() ? 0 : it->second;
}

Resource::~Resource() {
    if (place) placement_forget(this);
}

}  // namespace fl::d3d
