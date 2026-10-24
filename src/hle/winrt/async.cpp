#include "hle/winrt/async.h"

#include <atomic>

#include "common/log.h"

namespace fl::winrt {
namespace {

constexpr int S = kInspectableSlots;
constexpr GUID IID_IAsyncInfo = {0x00000036, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
std::atomic<uint32_t> g_next_id{1};

AsyncOperation* op(Iface* i) { return i->as<AsyncOperation>(); }

WINAPI HRESULT op_put_Completed(Iface* i, void* handler) {
    auto* o = op(i);
    if (o->handler) return static_cast<HRESULT>(0x80000018);  // E_ILLEGAL_DELEGATE_ASSIGNMENT
    if (!handler) return S_OK;
    com_add_ref(handler);
    o->handler = handler;
    AsyncStatus st = o->error < 0 ? kError : kCompleted;
    return call_slot<HRESULT>(handler, 3, static_cast<void*>(&o->iop), static_cast<int32_t>(st));
}
WINAPI HRESULT op_get_Completed(Iface* i, void** out) {
    auto* o = op(i);
    if (o->handler) com_add_ref(o->handler);
    *out = o->handler;
    return S_OK;
}
WINAPI HRESULT op_GetResults(Iface* i, void* out) {
    auto* o = op(i);
    if (o->error < 0) return o->error;
    if (o->write_result) o->write_result(out);
    return S_OK;
}

WINAPI HRESULT info_get_Id(Iface* i, uint32_t* out) {
    *out = op(i)->id;
    return S_OK;
}
WINAPI HRESULT info_get_Status(Iface* i, int32_t* out) {
    *out = op(i)->error < 0 ? kError : kCompleted;
    return S_OK;
}
WINAPI HRESULT info_get_ErrorCode(Iface* i, HRESULT* out) {
    *out = op(i)->error;
    return S_OK;
}
WINAPI HRESULT info_Cancel(Iface*) { return S_OK; }  // already finished
WINAPI HRESULT info_Close(Iface*) { return S_OK; }

Vtable& op_vt() {
    static Vtable v = Vtable("IAsyncOperation<T>", S + 3)
                          .set(S + 0, &op_put_Completed)
                          .set(S + 1, &op_get_Completed)
                          .set(S + 2, &op_GetResults);
    return v;
}
Vtable& info_vt() {
    static Vtable v = Vtable("IAsyncInfo", S + 5)
                          .set(S + 0, &info_get_Id)
                          .set(S + 1, &info_get_Status)
                          .set(S + 2, &info_get_ErrorCode)
                          .set(S + 3, &info_Cancel)
                          .set(S + 4, &info_Close);
    return v;
}

}  // namespace

AsyncOperation::AsyncOperation(const GUID& iid, std::function<void(void*)> w, HRESULT err)
    : Object(u"Windows.Foundation.IAsyncOperation`1"), iop(op_vt(), this), iinfo(info_vt(), this),
      write_result(std::move(w)), error(err), id(g_next_id++) {
    expose(iid, &iop);
    expose(IID_IAsyncInfo, &iinfo);
}

void* async_result_u32(const GUID& iid, uint32_t value) {
    auto* o = new AsyncOperation(iid, [value](void* out) { std::memcpy(out, &value, 4); });
    return o->default_iface_noref();
}

void* async_result_ptr(const GUID& iid, void* object) {
    auto* o = new AsyncOperation(iid, [object](void* out) {
        if (object) com_add_ref(object);
        *static_cast<void**>(out) = object;
    });
    return o->default_iface_noref();
}

void* async_failed(const GUID& iid, HRESULT error) {
    return (new AsyncOperation(iid, nullptr, error))->default_iface_noref();
}

}  // namespace fl::winrt
