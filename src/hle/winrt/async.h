// IAsyncOperation<T> / IAsyncAction that are already finished when handed out.
//   IAsyncOperation<T>: S+0 put_Completed(handler), S+1 get_Completed, S+2 GetResults(T*)
//   IAsyncInfo {00000036-...}: S+0 get_Id, S+1 get_Status, S+2 get_ErrorCode, S+3 Cancel, S+4 Close
// The completion handler (IUnknown-based, slot 3 = Invoke(op, status)) runs
// right away inside put_Completed, as WinRT does for finished operations.
#pragma once

#include <cstring>
#include <functional>

#include "hle/winrt/object.h"

namespace fl::winrt {

enum AsyncStatus : int32_t { kStarted = 0, kCompleted = 1, kCanceled = 2, kError = 3 };

struct AsyncOperation : Object {
    // `iid`: the IAsyncOperation<T> (or IAsyncAction) IID. `write_result(out)`
    // stores T into GetResults' out pointer (nullptr for actions).
    AsyncOperation(const GUID& iid, std::function<void(void*)> write_result, HRESULT error = S_OK);
    Iface iop;
    Iface iinfo;
    std::function<void(void*)> write_result;
    HRESULT error;
    void* handler = nullptr;
    uint32_t id;
    bool pending = false;  // never completes (Started forever): see async_pending
};

// Common shortcuts. Each returns a new operation's interface pointer (owning).
void* async_result_u32(const GUID& iid, uint32_t value);
void* async_result_ptr(const GUID& iid, void* object);  // takes ownership of one reference
void* async_failed(const GUID& iid, HRESULT error);
// An operation that stays Started forever: its handler is stored, never called.
// For requests a disconnected console would leave waiting.
void* async_pending(const GUID& iid);

}  // namespace fl::winrt
