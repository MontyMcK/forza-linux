#include "hle/winrt/collections.h"

namespace fl::winrt {
namespace {

constexpr int S = kInspectableSlots;
constexpr HRESULT E_BOUNDS = static_cast<HRESULT>(0x8000000B);

VectorView* self(Iface* i) { return i->as<VectorView>(); }

WINAPI HRESULT vv_GetAt(Iface* i, uint32_t index, void** out) {
    auto& v = self(i)->items;
    if (index >= v.size()) {
        *out = nullptr;
        return E_BOUNDS;
    }
    if (v[index]) com_add_ref(v[index]);
    *out = v[index];
    return S_OK;
}
WINAPI HRESULT vv_get_Size(Iface* i, uint32_t* out) {
    *out = static_cast<uint32_t>(self(i)->items.size());
    return S_OK;
}
WINAPI HRESULT vv_IndexOf(Iface* i, void* value, uint32_t* index, uint8_t* found) {
    auto& v = self(i)->items;
    for (uint32_t k = 0; k < v.size(); k++)
        if (v[k] == value) {
            *index = k;
            *found = 1;
            return S_OK;
        }
    *index = 0;
    *found = 0;
    return S_OK;
}
WINAPI HRESULT vv_GetMany(Iface* i, uint32_t start, uint32_t capacity, void** items, uint32_t* count) {
    auto& v = self(i)->items;
    uint32_t n = 0;
    for (uint32_t k = start; k < v.size() && n < capacity; k++, n++) {
        if (v[k]) com_add_ref(v[k]);
        items[n] = v[k];
    }
    *count = n;
    return S_OK;
}

Vtable& vt() {
    static Vtable v = Vtable("IVectorView<T>", S + 4)
                          .set(S + 0, &vv_GetAt)
                          .set(S + 1, &vv_get_Size)
                          .set(S + 2, &vv_IndexOf)
                          .set(S + 3, &vv_GetMany);
    return v;
}

}  // namespace

VectorView::VectorView(const GUID& iid, std::vector<void*> v)
    : Object(u"Windows.Foundation.Collections.IVectorView`1"), iview(vt(), this), items(std::move(v)) {
    expose(iid, &iview);
}

VectorView::~VectorView() {
    for (void* p : items)
        if (p) com_release(p);
}

}  // namespace fl::winrt
