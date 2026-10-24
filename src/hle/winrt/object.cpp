#include "hle/winrt/object.h"

#include "common/log.h"
#include "common/wstr.h"
#include "loader/imports.h"

namespace fl::winrt {
namespace {

Iface* to_iface(void* p) { return static_cast<Iface*>(p); }

WINAPI HRESULT obj_QueryInterface(void* self, const GUID& iid, void** out) {
    return to_iface(self)->owner->query(iid, out);
}
WINAPI ULONG obj_AddRef(void* self) { return to_iface(self)->owner->add_ref(); }
WINAPI ULONG obj_Release(void* self) { return to_iface(self)->owner->release(); }

WINAPI HRESULT obj_GetIids(void* self, ULONG* count, GUID** iids) {
    *count = 0;
    *iids = nullptr;
    return S_OK;
}

WINAPI HRESULT obj_GetRuntimeClassName(void* self, HSTRING* name) {
    return make_hstring(to_iface(self)->owner->class_name, name);
}

WINAPI HRESULT obj_GetTrustLevel(void* self, TrustLevel* level) {
    *level = BaseTrust;
    return S_OK;
}

}  // namespace

Vtable::Vtable(const char* iface_name, int slots, bool inspectable) : name_(iface_name), slots_(slots) {
    for (int i = 0; i < slots; i++) slots_[i] = loader::make_named_trap(name_, "slot " + std::to_string(i));
    slots_[0] = reinterpret_cast<void*>(&obj_QueryInterface);
    slots_[1] = reinterpret_cast<void*>(&obj_AddRef);
    slots_[2] = reinterpret_cast<void*>(&obj_Release);
    if (inspectable) {
        slots_[3] = reinterpret_cast<void*>(&obj_GetIids);
        slots_[4] = reinterpret_cast<void*>(&obj_GetRuntimeClassName);
        slots_[5] = reinterpret_cast<void*>(&obj_GetTrustLevel);
    }
}

Vtable& Vtable::set(int slot, void* fn) {
    if (slot < 0 || slot >= static_cast<int>(slots_.size())) fl::fatal("%s: slot %d out of range", name_.c_str(), slot);
    slots_[slot] = fn;
    return *this;
}

HRESULT Object::query(const GUID& iid, void** out) {
    if (!out) return E_POINTER;
    if (iid == IID_IUnknown || iid == IID_IInspectable || iid == IID_IAgileObject) {
        *out = ifaces.front().iface;
        add_ref();
        return S_OK;
    }
    for (auto& e : ifaces) {
        if (e.iid == iid) {
            *out = e.iface;
            add_ref();
            return S_OK;
        }
    }
    *out = nullptr;
    // IMarshal / IWeakReferenceSource / ICustomPropertyProvider probes are routine.
    FL_LOG(winrt, "%s::QueryInterface(%s) -> E_NOINTERFACE", utf16_to_utf8(class_name).c_str(), guid_str(iid).c_str());
    return E_NOINTERFACE;
}

namespace {
WINAPI HRESULT factory_ActivateInstance(Iface* self, void** out) {
    *out = self->as<ActivationFactory>()->make();
    return *out ? S_OK : E_FAIL;
}
Vtable& factory_vt() {
    static Vtable v = Vtable("IActivationFactory", kInspectableSlots + 1).set(kInspectableSlots, &factory_ActivateInstance);
    return v;
}
}  // namespace

ActivationFactory::ActivationFactory(const char16_t* class_name, void* (*m)())
    : Object(class_name), ifactory(factory_vt(), this), make(m) {
    expose(IID_IActivationFactory, &ifactory);
}

HRESULT EventSource::add(void* handler, EventToken* token) {
    if (!handler || !token) return E_INVALIDARG;
    com_add_ref(handler);
    std::lock_guard lock(m_);
    token->value = next_++;
    handlers_.push_back({token->value, handler});
    return S_OK;
}

HRESULT EventSource::remove(EventToken token) {
    void* victim = nullptr;
    {
        std::lock_guard lock(m_);
        for (auto it = handlers_.begin(); it != handlers_.end(); ++it) {
            if (it->first == token.value) {
                victim = it->second;
                handlers_.erase(it);
                break;
            }
        }
    }
    if (victim) com_release(victim);
    return S_OK;
}

void EventSource::invoke(void* sender, void* args) {
    std::vector<void*> copy;
    {
        std::lock_guard lock(m_);
        for (auto& [tok, h] : handlers_) {
            com_add_ref(h);
            copy.push_back(h);
        }
    }
    for (void* h : copy) {
        HRESULT hr = call_slot<HRESULT>(h, 3, sender, args);
        if (hr < 0) FL_ERROR("event handler returned 0x%08x", static_cast<uint32_t>(hr));
        com_release(h);
    }
}

bool EventSource::empty() {
    std::lock_guard lock(m_);
    return handlers_.empty();
}

}  // namespace fl::winrt
