// vccorlib110: the C++/CX runtime support library.
// Only functions with obvious semantics live here so far; the rest stay
// UNIMPLEMENTED traps until the game hits them.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <mutex>
#include <vector>
#include <cstdint>
#include <cstring>

#include "common/log.h"
#include "common/winabi.h"
#include "common/wstr.h"
#include "hle/registry.h"
#include "hle/winrt/activation.h"
#include "hle/winrt/object.h"
#include "loader/pe_image.h"

namespace fl::vccorlib {
namespace {

// HRESULT Platform::Details::InitializeData(int)
WINAPI HRESULT v_InitializeData(int x) {
    FL_LOG(winrt, "Platform::Details::InitializeData(%d)", x);
    return S_OK;
}

WINAPI void v_UninitializeData(int x) {}

// Ref-class object storage.
WINAPI void* v_HeapAllocate(size_t n) { return calloc(1, n ? n : 1); }
WINAPI void v_HeapFree(void* p) { free(p); }
WINAPI void* v_HeapAllocateException(size_t n) { return calloc(1, n ? n : 1); }
WINAPI void v_HeapFreeException(void* p) { free(p); }

// Platform::Object::Object(): the base constructor has no state of its own.
WINAPI void* v_ObjectCtor(void* self) { return self; }

// ---- value-type ToString (this = pointer to the value) ----
// Formats match vccorlib's ("true"/"false", %I32d..., lowercase braced GUID).
winrt::HSTRING hstring_of(const char* s) {
    std::u16string w(s, s + strlen(s));
    winrt::HSTRING out = nullptr;
    winrt::make_hstring(w.c_str(), &out);
    return out;
}
WINAPI winrt::HSTRING v_BooleanToString(const bool* v) { return hstring_of(*v ? "true" : "false"); }
WINAPI winrt::HSTRING v_Int32ToString(const int32_t* v) { return hstring_of(std::to_string(*v).c_str()); }
WINAPI winrt::HSTRING v_UInt16ToString(const uint16_t* v) { return hstring_of(std::to_string(*v).c_str()); }
WINAPI winrt::HSTRING v_UInt32ToString(const uint32_t* v) { return hstring_of(std::to_string(*v).c_str()); }
WINAPI winrt::HSTRING v_UInt64ToString(const uint64_t* v) { return hstring_of(std::to_string(*v).c_str()); }
WINAPI winrt::HSTRING v_GuidToString(const GUID* g) {
    char buf[64];
    snprintf(buf, sizeof buf, "{%08x-%04hx-%04hx-%02hhx%02hhx-%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx}", g->Data1, g->Data2,
             g->Data3, g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6],
             g->Data4[7]);
    return hstring_of(buf);
}

// __abi_ObjectToString(Object^, bool): the object's own ToString through
// C++/CX's printable interface {DE0CBAEB-8065-4A45-96B1-C9D443F9BAB3} (slot 6),
// else its runtime class name (IInspectable slot 4).
constexpr GUID IID_abi_IPrintable = {0xDE0CBAEB, 0x8065, 0x4A45, {0x96, 0xB1, 0xC9, 0xD4, 0x43, 0xF9, 0xBA, 0xB3}};
WINAPI winrt::HSTRING v_ObjectToString(void* obj, bool) {
    if (!obj) return nullptr;
    winrt::HSTRING out = nullptr;
    void* printable = nullptr;
    if (winrt::call_slot<HRESULT>(obj, 0, &IID_abi_IPrintable, &printable) >= 0 && printable) {
        winrt::call_slot<HRESULT>(printable, 6, &out);
        winrt::com_release(printable);
        return out;
    }
    winrt::call_slot<HRESULT>(obj, 4, &out);
    return out;
}
WINAPI winrt::HSTRING v_EnumToString(void* self) { return v_ObjectToString(self, true); }

// ---- Platform::Type (from __abi_make_type_id) ----
// __abi_type_descriptor = {const wchar_t* name; int typeId}. Callers only use
// the exported members (get_FullName) and IInspectable on it.
struct TypeDescriptor {
    const char16_t* name;
    int32_t type_id;
};
struct Type : winrt::Object {
    const TypeDescriptor* desc;
    static winrt::Vtable& vt() {
        static winrt::Vtable v = winrt::Vtable("Platform::Type", winrt::kInspectableSlots);
        return v;
    }
    winrt::Iface iface{vt(), this};
    explicit Type(const TypeDescriptor* d) : Object(u"Platform.Type"), desc(d) { expose(winrt::IID_IUnknown, &iface); }
};
WINAPI void* v_MakeTypeId(const TypeDescriptor* d) {
    static std::mutex m;
    static std::vector<Type*> types;  // one Type per descriptor, kept alive
    std::lock_guard lock(m);
    for (Type* t : types)
        if (t->desc == d) return t->default_iface();
    types.push_back(new Type(d));
    return types.back()->default_iface();
}
WINAPI winrt::HSTRING v_TypeGetFullName(winrt::Iface* self) {
    winrt::HSTRING out = nullptr;
    winrt::make_hstring(self->as<Type>()->desc->name, &out);
    return out;
}

// ---- object context (CoGetObjectContext) ----
// One process-wide context. IContextCallback::ContextCallback (slot 3) runs the
// callback on the calling thread: we have no apartments to switch between.
constexpr GUID IID_IContextCallback = {0x000001DA, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
struct ObjectContext : winrt::Object {
    static WINAPI HRESULT ContextCallback(winrt::Iface*, HRESULT(WINAPI* fn)(void*), void* data, const GUID*, int,
                                          void*) {
        return fn(data);
    }
    static winrt::Vtable& vt() {
        static winrt::Vtable v = winrt::Vtable("IContextCallback", 4, false).set(3, &ContextCallback);
        return v;
    }
    winrt::Iface iface{vt(), this};
    ObjectContext() : Object(u"ObjectContext") {
        expose(winrt::IID_IUnknown, &iface);
        expose(IID_IContextCallback, &iface);
    }
};
WINAPI void* v_GetObjectContext() {
    static ObjectContext* ctx = new ObjectContext();
    return ctx->default_iface();
}

// TerminateModule(ModuleBase*): module unload; nothing to tear down.
WINAPI bool v_TerminateModule(void*) { return true; }

// Platform::Object::Equals(Object^): COM identity — both sides' IUnknown
// pointers compared (the game uses it to match a controller's user against its
// user list, rva 0x124a914).
WINAPI bool v_ObjectEquals(void* self, void* other) {
    if (!other) return false;
    if (self == other) return true;
    void *a = nullptr, *b = nullptr;
    winrt::call_slot<HRESULT>(self, 0, &winrt::IID_IUnknown, &a);
    winrt::call_slot<HRESULT>(other, 0, &winrt::IID_IUnknown, &b);
    bool eq = a && a == b;
    if (a) winrt::com_release(a);
    if (b) winrt::com_release(b);
    return eq;
}

// wchar_t** Platform::Details::GetCmdArguments(int* argc)
char16_t g_arg0[] = u"Forza_Durango_Release_Final.exe";
char16_t* g_argv[] = {g_arg0, nullptr};
WINAPI char16_t** v_GetCmdArguments(int* argc) {
    *argc = 1;
    return g_argv;
}

// HRESULT GetActivationFactoryByPCWSTR(void* classId, Platform::Guid& iid, void** factory)
WINAPI HRESULT v_GetActivationFactoryByPCWSTR(const char16_t* class_id, const GUID* iid, void** factory) {
    HRESULT hr = winrt::get_activation_factory(class_id, iid, factory);
    if (hr < 0) {
        auto ret = reinterpret_cast<uint64_t>(__builtin_return_address(0));
        FL_ERROR("  (requested from game rva 0x%llx)",
                 static_cast<unsigned long long>(ret - reinterpret_cast<uint64_t>(loader::main_image().base)));
    }
    return hr;
}

// void* GetIBoxVtable(void* this) / GetIBoxArrayVtable(void* this):
// Boxed value constructors store the result at this+8, replacing the
// compiler-generated IPropertyValue vtable with the runtime's generic one.
// STUB: keep the compiler's vtable (already at this+8). Fine until the game
// queries IPropertyValue on a boxed value.
// Platform::Details::GetActivationFactory(Microsoft::WRL::Details::ModuleBase*,
// HSTRING, IActivationFactory**): how a C++/CX component DLL's
// DllGetActivationFactory finds a class. Reimplements vccorlib rva 0x20ce8 ->
// 0x1ff2c: the module's virtual slots 5/6 give the bounds of its table of
// creator entries; each entry is {+0 factory creator, +8 name function, ...,
// +0x20 server name}. The creator is called as (flags*, entry, iid, out) with
// flags = 5.
WINAPI HRESULT v_GetActivationFactoryModule(void* module, winrt::HSTRING class_id, void** out) {
    *out = nullptr;
    if (!class_id) return E_INVALIDARG;
    using RawBuffer = const char16_t*(WINAPI*)(winrt::HSTRING, uint32_t*);
    static auto raw = reinterpret_cast<RawBuffer>(hle::lookup("combase", "WindowsGetStringRawBuffer"));
    uint32_t len = 0;
    const char16_t* name = raw(class_id, &len);
    std::u16string want(name, len);
    using EntryPtr = void**(WINAPI*)(void*);
    void** vt = *static_cast<void***>(module);
    auto** begin = reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(reinterpret_cast<EntryPtr>(vt[5])(module)) + 8);
    auto** end = reinterpret_cast<uint8_t**>(reinterpret_cast<EntryPtr>(vt[6])(module));
    for (uint8_t** p = begin; p < end; p++) {
        uint8_t* entry = *p;
        if (!entry) continue;
        using NameFn = const char16_t*(WINAPI*)();
        const char16_t* n = (*reinterpret_cast<NameFn*>(entry + 8))();
        if (!n || want != n) continue;
        using Creator = HRESULT(WINAPI*)(uint32_t*, void*, const GUID*, void**);
        uint32_t flags = 5;
        HRESULT hr = (*reinterpret_cast<Creator*>(entry))(&flags, entry, &winrt::IID_IActivationFactory, out);
        FL_LOG(winrt, "C++/CX module factory for %s -> 0x%08x", utf16_to_utf8(want).c_str(), static_cast<uint32_t>(hr));
        return hr;
    }
    FL_ERROR("vccorlib: class %s not found in its module", utf16_to_utf8(want).c_str());
    return static_cast<HRESULT>(0x80040111);  // CLASS_E_CLASSNOTAVAILABLE
}

WINAPI void* v_GetIBoxVtable(void* self) {
    FL_STUB_ONCE("vccorlib GetIBoxVtable/GetIBoxArrayVtable: keeping compiler vtable");
    return *reinterpret_cast<void**>(static_cast<uint8_t*>(self) + 8);
}

// ---------------------------------------------------------------- weak references

constexpr GUID IID_IWeakReferenceSource = {0x00000038, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr GUID IID_IWeakReference = {0x00000037, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};

// Fallback for objects without IWeakReferenceSource: holds a strong reference.
// STUB: keeps the target alive forever, which is acceptable for app-lifetime objects.
struct StrongWeakRef : winrt::Object {
    void* target;
    static WINAPI HRESULT Resolve(winrt::Iface* self, const GUID& iid, void** out) {
        return winrt::call_slot<HRESULT>(self->as<StrongWeakRef>()->target, 0, &iid, out);
    }
    static winrt::Vtable& vt() {
        static winrt::Vtable v = winrt::Vtable("IWeakReference", 4, false).set(3, &Resolve);
        return v;
    }
    winrt::Iface iweak{vt(), this};
    explicit StrongWeakRef(void* t) : Object(u"WeakReference"), target(t) {
        winrt::com_add_ref(t);
        expose(IID_IWeakReference, &iweak);
    }
};

// __abi_IUnknown* Platform::Details::GetWeakReference(Platform::Object^ const)
WINAPI void* v_GetWeakReference(void* obj) {
    if (!obj) return nullptr;
    void* source = nullptr;
    if (winrt::call_slot<HRESULT>(obj, 0, &IID_IWeakReferenceSource, &source) >= 0 && source) {
        void* weak = nullptr;
        HRESULT hr = winrt::call_slot<HRESULT>(source, 3, &weak);  // IWeakReferenceSource::GetWeakReference
        winrt::com_release(source);
        if (hr >= 0) return weak;
    }
    FL_STUB_ONCE("GetWeakReference: object lacks IWeakReferenceSource, using a strong reference");
    return &(new StrongWeakRef(obj))->iweak;
}

// Platform::Object^ Platform::Details::ResolveWeakReference(const GUID&, __abi_IUnknown**)
WINAPI void* v_ResolveWeakReference(const GUID* iid, void** weak) {
    if (!weak || !*weak) return nullptr;
    void* out = nullptr;
    if (winrt::call_slot<HRESULT>(*weak, 3, iid, &out) < 0) return nullptr;  // IWeakReference::Resolve
    return out;
}

// ---------------------------------------------------------------- delegates and events
// C++/CX event sources hold a pointer to an immutable "target array" that is
// replaced copy-on-write on add/remove. Raising an event does:
//   arr = EventSourceGetTargetArray(src, lock); n = ...Size(arr);
//   for i: ...Event(arr, i)->Invoke(...);  arr->Release();

WINAPI void* v_DelegateCtor(void* self) { return self; }

struct TargetArray : winrt::Object {
    std::vector<std::pair<int64_t, void*>> targets;  // (token, delegate), delegates AddRef'd
    static winrt::Vtable& vt() {
        static winrt::Vtable v = winrt::Vtable("EventTargetArray", 3, false);
        return v;
    }
    winrt::Iface iunk{vt(), this};
    TargetArray() : Object(u"EventTargetArray") { expose(winrt::IID_IUnknown, &iunk); }
    ~TargetArray() override {
        for (auto& [tok, d] : targets) winrt::com_release(d);
    }
};

std::mutex g_event_mutex;
std::atomic<int64_t> g_next_token{1};

TargetArray* arr_of(void* p) { return p ? static_cast<winrt::Iface*>(p)->as<TargetArray>() : nullptr; }

WINAPI void v_EventSourceInitialize(void** src) { *src = nullptr; }

WINAPI void v_EventSourceUninitialize(void** src) {
    std::lock_guard lock(g_event_mutex);
    if (TargetArray* a = arr_of(*src)) a->release();
    *src = nullptr;
}

struct EventToken {
    int64_t value;
};

WINAPI EventToken v_EventSourceAdd(void** src, void* lock_, void* delegate) {
    std::lock_guard lock(g_event_mutex);
    auto* next = new TargetArray();
    if (TargetArray* old = arr_of(*src)) {
        next->targets = old->targets;
        for (auto& [tok, d] : next->targets) winrt::com_add_ref(d);
        old->release();
    }
    int64_t token = g_next_token++;
    winrt::com_add_ref(delegate);
    next->targets.push_back({token, delegate});
    *src = &next->iunk;
    return {token};
}

WINAPI void v_EventSourceRemove(void** src, void* lock_, EventToken token) {
    std::lock_guard lock(g_event_mutex);
    TargetArray* old = arr_of(*src);
    if (!old) return;
    auto* next = new TargetArray();
    for (auto& [tok, d] : old->targets) {
        if (tok == token.value) continue;
        winrt::com_add_ref(d);
        next->targets.push_back({tok, d});
    }
    old->release();
    if (next->targets.empty()) {
        next->release();
        *src = nullptr;
    } else {
        *src = &next->iunk;
    }
}

WINAPI void* v_EventSourceGetTargetArray(void* src, void* lock_) {
    std::lock_guard lock(g_event_mutex);
    void* p = *static_cast<void**>(src);
    if (TargetArray* a = arr_of(p)) a->add_ref();
    return p;
}

WINAPI unsigned v_EventSourceGetTargetArraySize(void* arr) {
    TargetArray* a = arr_of(arr);
    return a ? static_cast<unsigned>(a->targets.size()) : 0;
}

WINAPI void* v_EventSourceGetTargetArrayEvent(void* arr, unsigned i) {
    TargetArray* a = arr_of(arr);
    return a && i < a->targets.size() ? a->targets[i].second : nullptr;
}

// ---------------------------------------------------------------- misc runtime helpers

// HRESULT GetIidsFn(int count, unsigned long* out_count, const GUID* iids, Platform::Guid** out)
WINAPI HRESULT v_GetIidsFn(int count, ULONG* out_count, const GUID* iids, GUID** out) {
    *out_count = static_cast<ULONG>(count);
    *out = static_cast<GUID*>(malloc(sizeof(GUID) * (count ? count : 1)));
    memcpy(*out, iids, sizeof(GUID) * count);
    return S_OK;
}

WINAPI HRESULT v_ReleaseInContextImpl(void* obj, void* ctx) {
    if (obj) winrt::com_release(obj);
    return S_OK;
}

// HRESULT GetProxyImpl(IUnknown* obj, const GUID& iid, IUnknown* ctx, IUnknown** proxy)
WINAPI HRESULT v_GetProxyImpl(void* obj, const GUID* iid, void* ctx, void** out) {
    return winrt::call_slot<HRESULT>(obj, 0, iid, out);
}

}  // namespace

HLE_EXPORT_AS(vccorlib110, "??0Delegate@Platform@@QE$AAA@XZ", v_DelegateCtor);
HLE_EXPORT_AS(vccorlib110, "?EventSourceInitialize@Details@Platform@@YAXPEAPEAX@Z", v_EventSourceInitialize);
HLE_EXPORT_AS(vccorlib110, "?EventSourceUninitialize@Details@Platform@@YAXPEAPEAX@Z", v_EventSourceUninitialize);
HLE_EXPORT_AS(vccorlib110,
              "?EventSourceAdd@Details@Platform@@YA?AVEventRegistrationToken@Foundation@Windows@@PEAPEAXPEAUEventLock@12@PE$AAVDelegate@2@@Z",
              v_EventSourceAdd);
HLE_EXPORT_AS(vccorlib110,
              "?EventSourceRemove@Details@Platform@@YAXPEAPEAXPEAUEventLock@12@VEventRegistrationToken@Foundation@Windows@@@Z",
              v_EventSourceRemove);
HLE_EXPORT_AS(vccorlib110, "?EventSourceGetTargetArray@Details@Platform@@YAPEAXPEAXPEAUEventLock@12@@Z",
              v_EventSourceGetTargetArray);
HLE_EXPORT_AS(vccorlib110, "?EventSourceGetTargetArraySize@Details@Platform@@YAIPEAX@Z", v_EventSourceGetTargetArraySize);
HLE_EXPORT_AS(vccorlib110, "?EventSourceGetTargetArrayEvent@Details@Platform@@YAPEAXPEAXI@Z",
              v_EventSourceGetTargetArrayEvent);
HLE_EXPORT_AS(vccorlib110, "?GetIidsFn@@YAJHPEAKPEBU__s_GUID@@PEAPEAVGuid@Platform@@@Z", v_GetIidsFn);
HLE_EXPORT_AS(vccorlib110, "?ReleaseInContextImpl@Details@Platform@@YAJPEAUIUnknown@@0@Z", v_ReleaseInContextImpl);
HLE_EXPORT_AS(vccorlib110, "?GetProxyImpl@Details@Platform@@YAJPEAUIUnknown@@AEBU_GUID@@0PEAPEAU3@@Z", v_GetProxyImpl);
HLE_EXPORT_AS(vccorlib110, "?GetWeakReference@Details@Platform@@YAPEAU__abi_IUnknown@@QE$ADVObject@2@@Z",
              v_GetWeakReference);
HLE_EXPORT_AS(vccorlib110, "?ResolveWeakReference@Details@Platform@@YAPE$AAVObject@2@AEBU_GUID@@PEAPEAU__abi_IUnknown@@@Z",
              v_ResolveWeakReference);
HLE_EXPORT_AS(vccorlib110, "?GetIBoxVtable@Details@Platform@@YAPEAXPEAX@Z", v_GetIBoxVtable);
HLE_EXPORT_AS(vccorlib110, "?GetIBoxArrayVtable@Details@Platform@@YAPEAXPEAX@Z", v_GetIBoxVtable);
HLE_EXPORT_AS(vccorlib110, "?InitializeData@Details@Platform@@YAJH@Z", v_InitializeData);
HLE_EXPORT_AS(vccorlib110, "?UninitializeData@Details@Platform@@YAXH@Z", v_UninitializeData);
HLE_EXPORT_AS(vccorlib110, "?Allocate@Heap@Details@Platform@@SAPEAX_K@Z", v_HeapAllocate);
HLE_EXPORT_AS(vccorlib110, "?Free@Heap@Details@Platform@@SAXPEAX@Z", v_HeapFree);
HLE_EXPORT_AS(vccorlib110, "?AllocateException@Heap@Details@Platform@@SAPEAX_K@Z", v_HeapAllocateException);
HLE_EXPORT_AS(vccorlib110, "?FreeException@Heap@Details@Platform@@SAXPEAX@Z", v_HeapFreeException);
HLE_EXPORT_AS(vccorlib110, "??0Object@Platform@@QE$AAA@XZ", v_ObjectCtor);
HLE_EXPORT_AS(vccorlib110, "?Equals@Object@Platform@@QE$AAA_NPE$AAV12@@Z", v_ObjectEquals);
HLE_EXPORT_AS(vccorlib110, "?ToString@Boolean@Platform@@QEAAPE$AAVString@2@XZ", v_BooleanToString);
HLE_EXPORT_AS(vccorlib110, "?ToString@int32@default@@QEAAPE$AAVString@Platform@@XZ", v_Int32ToString);
HLE_EXPORT_AS(vccorlib110, "?ToString@uint16@default@@QEAAPE$AAVString@Platform@@XZ", v_UInt16ToString);
HLE_EXPORT_AS(vccorlib110, "?ToString@uint32@default@@QEAAPE$AAVString@Platform@@XZ", v_UInt32ToString);
HLE_EXPORT_AS(vccorlib110, "?ToString@uint64@default@@QEAAPE$AAVString@Platform@@XZ", v_UInt64ToString);
HLE_EXPORT_AS(vccorlib110, "?ToString@Guid@Platform@@QEAAPE$AAVString@2@XZ", v_GuidToString);
HLE_EXPORT_AS(vccorlib110, "?ToString@Enum@Platform@@QE$AAAPE$AAVString@2@XZ", v_EnumToString);
HLE_EXPORT_AS(vccorlib110, "?__abi_ObjectToString@__abi_details@@YAPE$AAVString@Platform@@PE$AAVObject@3@_N@Z",
              v_ObjectToString);
HLE_EXPORT_AS(vccorlib110, "?__abi_make_type_id@@YAPE$AAVType@Platform@@AEBU__abi_type_descriptor@@@Z", v_MakeTypeId);
HLE_EXPORT_AS(vccorlib110, "?get@FullName@Type@Platform@@QE$AAAPE$AAVString@3@XZ", v_TypeGetFullName);
HLE_EXPORT_AS(vccorlib110, "?GetObjectContext@Details@Platform@@YAPEAUIUnknown@@XZ", v_GetObjectContext);
HLE_EXPORT_AS(vccorlib110, "?TerminateModule@Details@Platform@@YA_NPEAVModuleBase@1WRL@Microsoft@@@Z", v_TerminateModule);
HLE_EXPORT_AS(vccorlib110, "?GetCmdArguments@Details@Platform@@YAPEAPEA_WPEAH@Z", v_GetCmdArguments);
HLE_EXPORT_AS(vccorlib110,
              "?GetActivationFactory@Details@Platform@@YAJPEAVModuleBase@1WRL@Microsoft@@PEAUHSTRING__@@PEAPEAUIActivationFactory@@@Z",
              v_GetActivationFactoryModule);
HLE_EXPORT_AS(vccorlib110, "?GetActivationFactoryByPCWSTR@@YAJPEAXAEAVGuid@Platform@@PEAPEAX@Z",
              v_GetActivationFactoryByPCWSTR);

}  // namespace fl::vccorlib
