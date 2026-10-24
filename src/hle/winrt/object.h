// Dynamic WinRT/COM objects with runtime-built vtables.
//
// Why not C++ virtual classes: the interfaces we implement are large and are
// reverse-engineered slot by slot. Here every slot starts as a trap that
// reports "UNIMPLEMENTED <Interface> slot N", and we fill slots as we learn them.
//
//   static Vtable& vt() {
//       static Vtable v = Vtable("ICoreWindow", kInspectableSlots + 52)
//           .set(kInspectableSlots + 1, &get_Bounds);
//       return v;
//   }
//   struct CoreWindow : Object { Iface iwindow{vt(), this}; CoreWindow() { expose(IID_ICoreWindow, &iwindow); } };
//
// Methods are free functions: HRESULT WINAPI get_Bounds(Iface* self, Rect* out)
// and recover the object with self->as<CoreWindow>().
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "common/winabi.h"
#include "hle/winrt/com.h"

namespace fl::winrt {

constexpr int kUnknownSlots = 3;     // QueryInterface, AddRef, Release
constexpr int kInspectableSlots = 6; // + GetIids, GetRuntimeClassName, GetTrustLevel

class Vtable {
public:
    // `inspectable` = slots 3..5 are IInspectable's.
    Vtable(const char* iface_name, int slots, bool inspectable = true);
    Vtable& set(int slot, void* fn);
    template <typename F>
    Vtable& set(int slot, F* fn) {
        return set(slot, reinterpret_cast<void*>(fn));
    }
    void** table() { return slots_.data(); }

private:
    std::string name_;
    std::vector<void*> slots_;
};

struct Object;

// One interface pointer: what the game holds. `owner` lets methods find the object.
struct Iface {
    void** vtbl;
    Object* owner;
    Iface(Vtable& vt, Object* o) : vtbl(vt.table()), owner(o) {}
    template <typename T>
    T* as() {
        return static_cast<T*>(owner);
    }
};

struct Object {
    explicit Object(const char16_t* runtime_class) : class_name(runtime_class) {}
    virtual ~Object() = default;

    // Register an interface. The first one is also returned for IUnknown/IInspectable.
    void expose(const GUID& iid, Iface* iface) { ifaces.push_back({iid, iface}); }

    HRESULT query(const GUID& iid, void** out);
    ULONG add_ref() { return ++refs; }
    ULONG release() {
        ULONG r = --refs;
        if (r == 0) delete this;
        return r;
    }
    // The default interface pointer (what you hand to the game), AddRef'd.
    void* default_iface() {
        add_ref();
        return ifaces.front().iface;
    }
    // Same, handing over the creation reference (for objects made just now).
    void* default_iface_noref() { return ifaces.front().iface; }

    std::atomic<ULONG> refs{1};
    const char16_t* class_name;
    struct Entry {
        GUID iid;
        Iface* iface;
    };
    std::vector<Entry> ifaces;
};

// IActivationFactory for classes created with `ref new` (no statics):
// S+0 ActivateInstance(IInspectable**) calls `make`, which returns a new
// object's default interface (owning).
struct ActivationFactory : Object {
    ActivationFactory(const char16_t* class_name, void* (*make)());
    Iface ifactory;
    void* (*make)();
};

// Call slot `n` of a game-provided COM object (ms_abi).
template <typename R, typename... A>
R call_slot(void* obj, int n, A... args) {
    auto** vtbl = *static_cast<void***>(obj);
    return reinterpret_cast<R(WINAPI*)(void*, A...)>(vtbl[n])(obj, args...);
}

inline ULONG com_add_ref(void* obj) { return call_slot<ULONG>(obj, 1); }
inline ULONG com_release(void* obj) { return call_slot<ULONG>(obj, 2); }

// EventRegistrationToken is a struct holding one int64, passed by value.
struct EventToken {
    int64_t value;
};

// A WinRT event: a list of delegates. Delegates are game objects whose slot 3
// is Invoke(sender, args).
class EventSource {
public:
    HRESULT add(void* handler, EventToken* token);
    HRESULT remove(EventToken token);
    void invoke(void* sender, void* args);
    bool empty();

private:
    std::mutex m_;
    std::vector<std::pair<int64_t, void*>> handlers_;
    int64_t next_ = 1;
};

}  // namespace fl::winrt
