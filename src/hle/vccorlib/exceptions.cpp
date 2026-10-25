// C++/CX exceptions: Platform::Exception objects and the __abi_WinRTraise*
// functions that throw them as MSVC C++ exceptions of type `Platform::Xxx^`.
//
// Object layout follows vccorlib110 (reference, FINDINGS §5b): allocated as
// 0x98 bytes, object at +0x10, four interface pointers at +0x00/+0x08/+0x10/+0x18,
// a WINRTEXCEPTIONINFO at +0x20 (HRESULT +0x40, ThrowInfo +0x50, size +0x58 =
// 0x40) that the word just before the object points to, refcount at +0x60. The
// ThrowInfo is the exception's dynamic type: _CxxThrowException uses it when a
// hat is thrown through a static type (cxx_eh.cpp). The +0x10 interface's slot 9
// returns the HRESULT. Other methods (message strings) trap until something uses them.
//
// Catch matching compares type names, so our ThrowInfo uses the C++/CX
// decorated names (".PE$AAVException@Platform@@" ...). ThrowInfo/TypeDescriptor
// fields are 32-bit RVAs, so that metadata lives just after the game image.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <vector>

#include "common/log.h"
#include "common/winabi.h"
#include "hle/registry.h"
#include "hle/seh/seh.h"
#include "hle/winrt/com.h"
#include "loader/imports.h"
#include "loader/pe_image.h"

extern "C" void fl_cxx_throw_impl(void* obj, void* throw_info, uint64_t, uint64_t, fl::seh::Context* ctx);

namespace fl::vccorlib {
namespace {

// ---- exception objects ----

constexpr size_t kAllocSize = 0x98, kHeader = 0x10;
constexpr size_t kInfo = 0x20, kHResult = 0x40, kThrowInfo = 0x50, kInfoSize = 0x58, kRefs = 0x60, kOwned = 0x74;

uint8_t* obj_of(void* iface, size_t off) { return static_cast<uint8_t*>(iface) - off; }
HRESULT& hr_of(uint8_t* o) { return *reinterpret_cast<HRESULT*>(o + kHResult); }
std::atomic<int32_t>& refs_of(uint8_t* o) { return *reinterpret_cast<std::atomic<int32_t>*>(o + kRefs); }

template <size_t Off>
WINAPI HRESULT ex_QueryInterface(void* self, const GUID& iid, void** out) {
    uint8_t* o = obj_of(self, Off);
    if (iid == winrt::IID_IUnknown || iid == winrt::IID_IInspectable) {
        refs_of(o)++;
        *out = o;
        return S_OK;
    }
    FL_LOG(seh, "Platform::Exception::QueryInterface(%s) -> E_NOINTERFACE", winrt::guid_str(iid).c_str());
    *out = nullptr;
    return E_NOINTERFACE;
}
template <size_t Off>
WINAPI ULONG ex_AddRef(void* self) {
    return ++refs_of(obj_of(self, Off));
}
template <size_t Off>
WINAPI ULONG ex_Release(void* self) {
    uint8_t* o = obj_of(self, Off);
    int32_t r = --refs_of(o);
    if (r == 0 && o[kOwned]) free(o - kHeader);  // only objects we allocated
    return r;
}
template <size_t Off>
WINAPI HRESULT ex_GetIids(void* self, ULONG* n, GUID** iids) {
    *n = 0;
    *iids = nullptr;
    return S_OK;
}
template <size_t Off>
WINAPI HRESULT ex_GetRuntimeClassName(void* self, winrt::HSTRING* out) {
    return winrt::make_hstring(u"Platform.Exception", out);
}
template <size_t Off>
WINAPI HRESULT ex_GetTrustLevel(void* self, int32_t* out) {
    *out = 0;
    return S_OK;
}
WINAPI HRESULT ex_get_HResult(void* self) { return hr_of(obj_of(self, 0x10)); }

template <size_t Off>
void** make_vtable(int slots, const char* name) {
    auto** t = new void*[slots];
    for (int i = 0; i < slots; i++)
        t[i] = loader::make_named_trap(name, "slot " + std::to_string(i));
    t[0] = reinterpret_cast<void*>(&ex_QueryInterface<Off>);
    t[1] = reinterpret_cast<void*>(&ex_AddRef<Off>);
    t[2] = reinterpret_cast<void*>(&ex_Release<Off>);
    t[3] = reinterpret_cast<void*>(&ex_GetIids<Off>);
    t[4] = reinterpret_cast<void*>(&ex_GetRuntimeClassName<Off>);
    t[5] = reinterpret_cast<void*>(&ex_GetTrustLevel<Off>);
    return t;
}

void* throw_info_for(const char* cls);
const char* class_for(HRESULT hr);

// cls: the Platform exception class this object is (null: COMException).
void init_exception(void* obj, HRESULT hr, bool owned, const char* cls) {
    static void** vt0 = make_vtable<0x00>(6, "Platform::Exception@+0");
    static void** vt8 = make_vtable<0x08>(8, "Platform::Exception@+8");
    static void** vt10 = [] {
        void** t = make_vtable<0x10>(10, "Platform::Exception@+0x10");
        t[9] = reinterpret_cast<void*>(&ex_get_HResult);
        return t;
    }();
    static void** vt18 = make_vtable<0x18>(8, "Platform::Exception@+0x18");
    auto* o = static_cast<uint8_t*>(obj);
    std::memset(o, 0, kAllocSize - kHeader);
    reinterpret_cast<void***>(o)[0] = vt0;
    reinterpret_cast<void***>(o)[1] = vt8;
    reinterpret_cast<void***>(o)[2] = vt10;
    reinterpret_cast<void***>(o)[3] = vt18;
    reinterpret_cast<uint8_t**>(o)[-1] = o + kInfo;  // vccorlib: the info pointer precedes the object
    hr_of(o) = hr;
    *reinterpret_cast<void**>(o + kThrowInfo) = throw_info_for(cls);
    *reinterpret_cast<uint32_t*>(o + kInfoSize) = 0x40;
    refs_of(o) = 1;
    o[kOwned] = owned;
}

void* new_exception(HRESULT hr) {
    auto* mem = static_cast<uint8_t*>(calloc(1, kAllocSize));
    init_exception(mem + kHeader, hr, true, class_for(hr));
    return mem + kHeader;
}

const char* class_for(HRESULT hr) {
    switch (static_cast<uint32_t>(hr)) {
        case 0x80070005: return "AccessDeniedException";
        case 0x8000000C: return "ChangedStateException";
        case 0x80040154: return "ClassNotRegisteredException";
        case 0x80010108: return "DisconnectedException";
        case 0x80004005: return "FailureException";
        case 0x80070057: return "InvalidArgumentException";
        case 0x80004002: return "InvalidCastException";
        case 0x80004001: return "NotImplementedException";
        case 0x80004003: return "NullReferenceException";
        case 0x80000013: return "ObjectDisposedException";
        case 0x80004004: return "OperationCanceledException";
        case 0x8000000B: return "OutOfBoundsException";
        case 0x8007000E: return "OutOfMemoryException";
        case 0x8001010E: return "WrongThreadException";
        default: return nullptr;
    }
}

// ---- MSVC EH metadata (RVAs relative to the main image) ----

struct Metadata {
    uint8_t* base = nullptr;  // main image base: RVAs are relative to it
    uint8_t* next = nullptr;
    uint8_t* end = nullptr;
    std::map<std::string, uint32_t> type_rvas;
    std::map<std::string, void*> throw_infos;
};
std::mutex g_meta_mutex;
Metadata g_meta;

uint32_t meta_alloc(size_t n) {
    Metadata& m = g_meta;
    if (!m.next) {
        loader::PeImage& img = loader::main_image();
        m.base = img.base;
        size_t size = 0x10000;
        uintptr_t want = (reinterpret_cast<uintptr_t>(img.base) + img.size + 0xffff) & ~uintptr_t(0xffff);
        void* p = MAP_FAILED;
        for (int i = 0; i < 64 && p == MAP_FAILED; i++, want += 0x10000)
            p = mmap(reinterpret_cast<void*>(want), size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == MAP_FAILED) fatal("vccorlib: no room for EH metadata after the game image");
        m.next = static_cast<uint8_t*>(p);
        m.end = m.next + size;
    }
    n = (n + 15) & ~size_t(15);
    if (m.next + n > m.end) fatal("vccorlib: EH metadata region full");
    uint8_t* p = m.next;
    m.next += n;
    return static_cast<uint32_t>(p - m.base);
}

uint32_t type_descriptor(const std::string& name) {
    auto it = g_meta.type_rvas.find(name);
    if (it != g_meta.type_rvas.end()) return it->second;
    uint32_t rva = meta_alloc(16 + name.size() + 1);  // {vftable, spare, name}
    std::memcpy(g_meta.base + rva + 16, name.c_str(), name.size() + 1);
    return g_meta.type_rvas[name] = rva;
}

// ThrowInfo for `Platform::<cls>^` (cls may be null: plain COMException).
void* throw_info_for(const char* cls) {
    std::lock_guard lock(g_meta_mutex);
    std::string key = cls ? cls : "COMException";
    auto it = g_meta.throw_infos.find(key);
    if (it != g_meta.throw_infos.end()) return it->second;
    std::vector<std::string> names;
    if (cls) names.push_back(std::string(".PE$AAV") + cls + "@Platform@@");
    names.push_back(".PE$AAVCOMException@Platform@@");
    names.push_back(".PE$AAVException@Platform@@");
    names.push_back(".PE$AAVObject@Platform@@");
    uint32_t cta = meta_alloc(4 + 4 * names.size());
    *reinterpret_cast<int32_t*>(g_meta.base + cta) = static_cast<int32_t>(names.size());
    for (size_t i = 0; i < names.size(); i++) {
        uint32_t ct = meta_alloc(28);
        auto* c = reinterpret_cast<uint32_t*>(g_meta.base + ct);
        c[0] = 1;  // CT_IsSimpleType: a pointer
        c[1] = type_descriptor(names[i]);
        c[2] = 0;                          // mdisp
        c[3] = static_cast<uint32_t>(-1);  // pdisp
        c[4] = 0;                          // vdisp
        c[5] = 8;                          // size
        c[6] = 0;                          // copy function
        reinterpret_cast<uint32_t*>(g_meta.base + cta + 4)[i] = ct;
    }
    uint32_t ti = meta_alloc(16);
    auto* t = reinterpret_cast<uint32_t*>(g_meta.base + ti);
    t[0] = 0;    // attributes
    t[1] = 0;    // no destructor: the thrown hat leaks one reference
    t[2] = 0;
    t[3] = cta;
    return g_meta.throw_infos[key] = g_meta.base + ti;
}

// ---- exports ----

WINAPI void* v_CreateException(HRESULT hr) { return new_exception(hr); }
WINAPI HRESULT v_ReCreateFromException(void* e) { return e ? hr_of(static_cast<uint8_t*>(e)) : S_OK; }

// The game allocates these with Heap::AllocateException (header included).
#define FL_EX_CTOR(fn, hr) \
    WINAPI void* fn(void* self) { return init_exception(self, static_cast<HRESULT>(hr), false, class_for(hr)), self; }
FL_EX_CTOR(v_ChangedStateCtor, 0x8000000C)
FL_EX_CTOR(v_DisconnectedCtor, 0x80010108)
FL_EX_CTOR(v_FailureCtor, 0x80004005)
FL_EX_CTOR(v_InvalidArgumentCtor, 0x80070057)
FL_EX_CTOR(v_NotImplementedCtor, 0x80004001)
FL_EX_CTOR(v_OutOfBoundsCtor, 0x8000000B)
FL_EX_CTOR(v_OutOfMemoryCtor, 0x8007000E)
FL_EX_CTOR(v_NullReferenceCtor, 0x80004003)
#undef FL_EX_CTOR
// COMException(hr) is a COMException whatever the HRESULT (vccorlib110 0x18470).
WINAPI void* v_COMExceptionCtor(void* self, HRESULT hr) { return init_exception(self, hr, false, nullptr), self; }

// Exception::Message: vccorlib FormatMessage()s the HRESULT; we have no
// system message table, so the text names the exception class and code.
WINAPI winrt::HSTRING v_ExceptionGetMessage(void* self) {
    HRESULT hr = hr_of(static_cast<uint8_t*>(self));
    const char* cls = class_for(hr);
    char buf[96];
    snprintf(buf, sizeof buf, "%s (HRESULT 0x%08X)", cls ? cls : "COMException", static_cast<uint32_t>(hr));
    std::u16string w(buf, buf + strlen(buf));
    winrt::HSTRING out = nullptr;
    winrt::make_hstring(w.c_str(), &out);
    return out;
}

[[noreturn]] WINAPI void v_FailFast() { fl::fatal("vccorlib: __abi_FailFast (C++/CX fail-fast)"); }

}  // namespace
}  // namespace fl::vccorlib

// Called (SysV) by the fl_WinRTraise_entry stub with the game caller's context.
extern "C" void fl_winrt_raise_impl(int32_t hr, uint64_t, uint64_t, uint64_t, fl::seh::Context* ctx) {
    using namespace fl::vccorlib;
    const char* cls = class_for(hr);
    FL_LOG(seh, "C++/CX raise %s (0x%08x)", cls ? cls : "COMException", static_cast<uint32_t>(hr));
    // The thrown object is the hat (a pointer); it must outlive the throw.
    auto** cell = new void*(new_exception(hr));
    fl_cxx_throw_impl(cell, throw_info_for(cls), 0, 0, ctx);
}

extern "C" {
void fl_WinRTraise_entry();
void fl_raise_AccessDenied();
void fl_raise_ChangedState();
void fl_raise_ClassNotRegistered();
void fl_raise_Disconnected();
void fl_raise_Failure();
void fl_raise_InvalidArgument();
void fl_raise_InvalidCast();
void fl_raise_NotImplemented();
void fl_raise_NullReference();
void fl_raise_ObjectDisposed();
void fl_raise_OperationCanceled();
void fl_raise_OutOfBounds();
void fl_raise_OutOfMemory();
void fl_raise_WrongThread();
}

namespace fl::vccorlib {
HLE_EXPORT_AS(vccorlib110, "?__abi_WinRTraiseCOMException@@YAXJ@Z", fl_WinRTraise_entry);
HLE_EXPORT_AS(vccorlib110, "?__abi_WinRTraiseAccessDeniedException@@YAXXZ", fl_raise_AccessDenied);
HLE_EXPORT_AS(vccorlib110, "?__abi_WinRTraiseChangedStateException@@YAXXZ", fl_raise_ChangedState);
HLE_EXPORT_AS(vccorlib110, "?__abi_WinRTraiseClassNotRegisteredException@@YAXXZ", fl_raise_ClassNotRegistered);
HLE_EXPORT_AS(vccorlib110, "?__abi_WinRTraiseDisconnectedException@@YAXXZ", fl_raise_Disconnected);
HLE_EXPORT_AS(vccorlib110, "?__abi_WinRTraiseFailureException@@YAXXZ", fl_raise_Failure);
HLE_EXPORT_AS(vccorlib110, "?__abi_WinRTraiseInvalidArgumentException@@YAXXZ", fl_raise_InvalidArgument);
HLE_EXPORT_AS(vccorlib110, "?__abi_WinRTraiseInvalidCastException@@YAXXZ", fl_raise_InvalidCast);
HLE_EXPORT_AS(vccorlib110, "?__abi_WinRTraiseNotImplementedException@@YAXXZ", fl_raise_NotImplemented);
HLE_EXPORT_AS(vccorlib110, "?__abi_WinRTraiseNullReferenceException@@YAXXZ", fl_raise_NullReference);
HLE_EXPORT_AS(vccorlib110, "?__abi_WinRTraiseObjectDisposedException@@YAXXZ", fl_raise_ObjectDisposed);
HLE_EXPORT_AS(vccorlib110, "?__abi_WinRTraiseOperationCanceledException@@YAXXZ", fl_raise_OperationCanceled);
HLE_EXPORT_AS(vccorlib110, "?__abi_WinRTraiseOutOfBoundsException@@YAXXZ", fl_raise_OutOfBounds);
HLE_EXPORT_AS(vccorlib110, "?__abi_WinRTraiseOutOfMemoryException@@YAXXZ", fl_raise_OutOfMemory);
HLE_EXPORT_AS(vccorlib110, "?__abi_WinRTraiseWrongThreadException@@YAXXZ", fl_raise_WrongThread);
HLE_EXPORT_AS(vccorlib110, "?CreateException@Exception@Platform@@SAPE$AAV12@H@Z", v_CreateException);
HLE_EXPORT_AS(vccorlib110, "?ReCreateException@Exception@Platform@@SAPE$AAV12@H@Z", v_CreateException);
HLE_EXPORT_AS(vccorlib110, "?ReCreateFromException@Details@Platform@@YAJPE$AAVException@2@@Z", v_ReCreateFromException);
HLE_EXPORT_AS(vccorlib110, "??0COMException@Platform@@QE$AAA@H@Z", v_COMExceptionCtor);
HLE_EXPORT_AS(vccorlib110, "??0ChangedStateException@Platform@@QE$AAA@XZ", v_ChangedStateCtor);
HLE_EXPORT_AS(vccorlib110, "??0NullReferenceException@Platform@@QE$AAA@XZ", v_NullReferenceCtor);
HLE_EXPORT_AS(vccorlib110, "?get@Message@Exception@Platform@@QE$AAAPE$AAVString@3@XZ", v_ExceptionGetMessage);
HLE_EXPORT_AS(vccorlib110, "?__abi_FailFast@@YAXXZ", v_FailFast);
HLE_EXPORT_AS(vccorlib110, "??0DisconnectedException@Platform@@QE$AAA@XZ", v_DisconnectedCtor);
HLE_EXPORT_AS(vccorlib110, "??0FailureException@Platform@@QE$AAA@XZ", v_FailureCtor);
HLE_EXPORT_AS(vccorlib110, "??0InvalidArgumentException@Platform@@QE$AAA@XZ", v_InvalidArgumentCtor);
HLE_EXPORT_AS(vccorlib110, "??0NotImplementedException@Platform@@QE$AAA@XZ", v_NotImplementedCtor);
HLE_EXPORT_AS(vccorlib110, "??0OutOfBoundsException@Platform@@QE$AAA@XZ", v_OutOfBoundsCtor);
HLE_EXPORT_AS(vccorlib110, "??0OutOfMemoryException@Platform@@QE$AAA@XZ", v_OutOfMemoryCtor);
}  // namespace fl::vccorlib
