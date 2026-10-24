// combase: HSTRING, COM initialization/apartments/context, task memory.
//
// The process is treated as one big MTA: every apartment/context question gets
// the same answer, and cross-apartment marshaling is a pass-through.

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <random>

#include "common/log.h"
#include "hle/winrt/activation.h"
#include "common/winabi.h"
#include "common/wstr.h"
#include "hle/registry.h"
#include "hle/winrt/com.h"

namespace fl::audio {
HRESULT create_mmdevice_enumerator(const GUID& clsid, const GUID& iid, void** out, bool* handled);
}

namespace fl::combase {
namespace {

using winrt::HSTRING;
using winrt::IUnknown;
using winrt::IID_IUnknown;
using winrt::guid_str;

// ---------------------------------------------------------------- HSTRING
// Heap strings own their buffer (stored right after the header).
// Reference ("fast-pass") strings live in the caller's 24-byte HSTRING_HEADER
// and point at the caller's buffer; they are never freed.

struct HStr {
    uint32_t flags;  // 1 = reference string
    uint32_t len;
    const char16_t* buf;
    std::atomic<int32_t> refs;
};
static_assert(sizeof(HStr) <= 24, "must fit in HSTRING_HEADER");
constexpr uint32_t kReference = 1;
const char16_t kEmpty[] = u"";

HStr* from(HSTRING h) { return reinterpret_cast<HStr*>(h); }

HSTRING make_heap(const char16_t* s, uint32_t len) {
    if (len == 0) return nullptr;  // the empty string is NULL
    auto* mem = static_cast<uint8_t*>(malloc(sizeof(HStr) + (len + 1) * 2));
    auto* h = reinterpret_cast<HStr*>(mem);
    auto* buf = reinterpret_cast<char16_t*>(mem + sizeof(HStr));
    memcpy(buf, s, len * 2);
    buf[len] = 0;
    h->flags = 0;
    h->len = len;
    h->buf = buf;
    new (&h->refs) std::atomic<int32_t>(1);
    return reinterpret_cast<HSTRING>(h);
}

WINAPI const char16_t* c_WindowsGetStringRawBuffer(HSTRING s, uint32_t* len);

WINAPI HRESULT c_WindowsConcatString(HSTRING a, HSTRING b, HSTRING* out) {
    uint32_t la, lb;
    const char16_t* pa = c_WindowsGetStringRawBuffer(a, &la);
    const char16_t* pb = c_WindowsGetStringRawBuffer(b, &lb);
    std::u16string s(pa, la);
    s.append(pb, lb);
    *out = make_heap(s.data(), static_cast<uint32_t>(s.size()));
    return S_OK;
}

WINAPI HRESULT c_WindowsStringHasEmbeddedNull(HSTRING s, BOOL* out) {
    uint32_t len;
    const char16_t* p = c_WindowsGetStringRawBuffer(s, &len);
    *out = FALSE_;
    for (uint32_t i = 0; i < len; i++)
        if (!p[i]) *out = TRUE_;
    return S_OK;
}

WINAPI HRESULT c_RoGetActivationFactory(HSTRING class_id, const GUID& iid, void** out) {
    uint32_t len;
    const char16_t* p = c_WindowsGetStringRawBuffer(class_id, &len);
    std::u16string name(p, len);
    return winrt::get_activation_factory(name.c_str(), &iid, out);
}

// Error reporting to the debugger/WER. Nothing to transform.
WINAPI BOOL c_RoTransformError(HRESULT, HRESULT, HSTRING) { return TRUE_; }

WINAPI HRESULT c_WindowsCreateString(const char16_t* s, uint32_t len, HSTRING* out) {
    if (!out || (!s && len)) return E_INVALIDARG;
    *out = make_heap(s, len);
    return S_OK;
}

WINAPI HRESULT c_WindowsCreateStringReference(const char16_t* s, uint32_t len, void* header, HSTRING* out) {
    if (!out || !header || (!s && len)) return E_INVALIDARG;
    if (s && s[len] != 0) return E_INVALIDARG;  // must be null-terminated
    if (len == 0) {
        *out = nullptr;
        return S_OK;
    }
    auto* h = static_cast<HStr*>(header);
    h->flags = kReference;
    h->len = len;
    h->buf = s;
    new (&h->refs) std::atomic<int32_t>(1);
    *out = reinterpret_cast<HSTRING>(h);
    return S_OK;
}

WINAPI HRESULT c_WindowsDeleteString(HSTRING s) {
    HStr* h = from(s);
    if (!h || (h->flags & kReference)) return S_OK;
    if (--h->refs == 0) free(h);
    return S_OK;
}

WINAPI HRESULT c_WindowsDuplicateString(HSTRING s, HSTRING* out) {
    if (!out) return E_INVALIDARG;
    HStr* h = from(s);
    if (!h) {
        *out = nullptr;
    } else if (h->flags & kReference) {
        *out = make_heap(h->buf, h->len);  // references must be copied
    } else {
        ++h->refs;
        *out = s;
    }
    return S_OK;
}

WINAPI uint32_t c_WindowsGetStringLen(HSTRING s) { return s ? from(s)->len : 0; }

WINAPI const char16_t* c_WindowsGetStringRawBuffer(HSTRING s, uint32_t* len) {
    if (len) *len = s ? from(s)->len : 0;
    return s ? from(s)->buf : kEmpty;
}

WINAPI BOOL c_WindowsIsStringEmpty(HSTRING s) { return !s || from(s)->len == 0; }

WINAPI HRESULT c_WindowsCompareStringOrdinal(HSTRING a, HSTRING b, int32_t* result) {
    if (!result) return E_INVALIDARG;
    uint32_t la, lb;
    const char16_t* pa = c_WindowsGetStringRawBuffer(a, &la);
    const char16_t* pb = c_WindowsGetStringRawBuffer(b, &lb);
    uint32_t n = std::min(la, lb);
    for (uint32_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) {
            *result = pa[i] < pb[i] ? -1 : 1;
            return S_OK;
        }
    }
    *result = la == lb ? 0 : (la < lb ? -1 : 1);
    return S_OK;
}

// ---------------------------------------------------------------- COM init / apartments

WINAPI HRESULT c_CoInitializeEx(void* reserved, DWORD flags) {
    FL_LOG(winrt, "CoInitializeEx(flags 0x%x)", flags);
    return S_OK;
}

constexpr int APTTYPE_MTA = 1;

WINAPI HRESULT c_CoGetApartmentType(int* type, int* qualifier) {
    *type = APTTYPE_MTA;
    *qualifier = 0;
    return S_OK;
}

WINAPI HRESULT c_CoGetContextToken(ULONG_PTR* token) {
    *token = 0x1000;  // one context for the whole process
    return S_OK;
}

// IContextCallback: run the callback directly (everything is one context).
constexpr GUID IID_IContextCallback = {0x000001DA, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};

struct ComCallData {
    DWORD dwDispid;
    DWORD dwReserved;
    void* pUserDefined;
};
using ContextCallbackFn = HRESULT(WINAPI*)(ComCallData* data);

struct IContextCallback : IUnknown {
    virtual HRESULT WINAPI ContextCallback(ContextCallbackFn fn, ComCallData* data, const GUID& iid, int method,
                                           IUnknown* unk) = 0;
};

struct ObjectContext final : IContextCallback {
    HRESULT WINAPI QueryInterface(const GUID& iid, void** out) override {
        if (iid == IID_IUnknown || iid == IID_IContextCallback) {
            *out = this;
            return S_OK;
        }
        FL_LOG(winrt, "ObjectContext::QueryInterface %s -> E_NOINTERFACE", guid_str(iid).c_str());
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG WINAPI AddRef() override { return 2; }   // static singleton
    ULONG WINAPI Release() override { return 1; }
    HRESULT WINAPI ContextCallback(ContextCallbackFn fn, ComCallData* data, const GUID& iid, int method,
                                   IUnknown* unk) override {
        return fn(data);
    }
};
ObjectContext g_context;

WINAPI HRESULT c_CoGetObjectContext(const GUID& iid, void** out) { return g_context.QueryInterface(iid, out); }

// Free-threaded marshaler: everything is agile here, so it is an inert object.
struct FreeThreadedMarshaler final : IUnknown {
    std::atomic<ULONG> refs{1};
    HRESULT WINAPI QueryInterface(const GUID& iid, void** out) override {
        if (iid == IID_IUnknown) {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG WINAPI AddRef() override { return ++refs; }
    ULONG WINAPI Release() override {
        ULONG r = --refs;
        if (!r) delete this;
        return r;
    }
};

WINAPI HRESULT c_CoCreateFreeThreadedMarshaler(IUnknown* outer, IUnknown** out) {
    *out = new FreeThreadedMarshaler();
    return S_OK;
}

// Marshaling across apartments is a pass-through: the "stream" is the
// interface pointer itself, AddRef'd.
WINAPI HRESULT c_CoMarshalInterThreadInterfaceInStream(const GUID& iid, IUnknown* unk, void** stream) {
    void* p = nullptr;
    HRESULT hr = unk->QueryInterface(iid, &p);
    if (hr < 0) return hr;
    *stream = p;
    return S_OK;
}

WINAPI HRESULT c_CoGetInterfaceAndReleaseStream(void* stream, const GUID& iid, void** out) {
    auto* unk = static_cast<IUnknown*>(stream);
    HRESULT hr = unk->QueryInterface(iid, out);
    unk->Release();
    return hr;
}

WINAPI HRESULT c_CoCreateInstance(const GUID& clsid, IUnknown* outer, DWORD ctx, const GUID& iid, void** out) {
    bool handled = false;
    HRESULT hr = audio::create_mmdevice_enumerator(clsid, iid, out, &handled);
    if (handled) return hr;
    FL_ERROR("UNIMPLEMENTED CoCreateInstance clsid %s iid %s", guid_str(clsid).c_str(), guid_str(iid).c_str());
    *out = nullptr;
    return static_cast<HRESULT>(0x80040154);  // REGDB_E_CLASSNOTREG
}

// ---------------------------------------------------------------- memory / misc

WINAPI void* c_CoTaskMemAlloc(size_t n) { return malloc(n ? n : 1); }
WINAPI void c_CoTaskMemFree(void* p) { free(p); }

WINAPI HRESULT c_CoCreateGuid(GUID* g) {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    uint64_t a = rng(), b = rng();
    memcpy(g, &a, 8);
    memcpy(reinterpret_cast<uint8_t*>(g) + 8, &b, 8);
    g->Data3 = static_cast<uint16_t>((g->Data3 & 0x0FFF) | 0x4000);  // version 4
    g->Data4[0] = static_cast<uint8_t>((g->Data4[0] & 0x3F) | 0x80);
    return S_OK;
}

}  // namespace

HLE_EXPORT(combase, WindowsCreateString, c_WindowsCreateString);
HLE_EXPORT(combase, WindowsConcatString, c_WindowsConcatString);
HLE_EXPORT(combase, WindowsStringHasEmbeddedNull, c_WindowsStringHasEmbeddedNull);
HLE_EXPORT(combase, RoGetActivationFactory, c_RoGetActivationFactory);
HLE_EXPORT(combase, RoTransformError, c_RoTransformError);
HLE_EXPORT(combase, WindowsCreateStringReference, c_WindowsCreateStringReference);
HLE_EXPORT(combase, WindowsDeleteString, c_WindowsDeleteString);
HLE_EXPORT(combase, WindowsDuplicateString, c_WindowsDuplicateString);
HLE_EXPORT(combase, WindowsGetStringLen, c_WindowsGetStringLen);
HLE_EXPORT(combase, WindowsGetStringRawBuffer, c_WindowsGetStringRawBuffer);
HLE_EXPORT(combase, WindowsIsStringEmpty, c_WindowsIsStringEmpty);
HLE_EXPORT(combase, WindowsCompareStringOrdinal, c_WindowsCompareStringOrdinal);
HLE_EXPORT(combase, CoInitializeEx, c_CoInitializeEx);
HLE_EXPORT(combase, CoGetApartmentType, c_CoGetApartmentType);
HLE_EXPORT(combase, CoGetContextToken, c_CoGetContextToken);
HLE_EXPORT(combase, CoGetObjectContext, c_CoGetObjectContext);
HLE_EXPORT(combase, CoCreateFreeThreadedMarshaler, c_CoCreateFreeThreadedMarshaler);
HLE_EXPORT(combase, CoMarshalInterThreadInterfaceInStream, c_CoMarshalInterThreadInterfaceInStream);
HLE_EXPORT(combase, CoGetInterfaceAndReleaseStream, c_CoGetInterfaceAndReleaseStream);
HLE_EXPORT(combase, CoCreateInstance, c_CoCreateInstance);
HLE_EXPORT(combase, CoTaskMemAlloc, c_CoTaskMemAlloc);
HLE_EXPORT(combase, CoTaskMemFree, c_CoTaskMemFree);
HLE_EXPORT(combase, CoCreateGuid, c_CoCreateGuid);

}  // namespace fl::combase

namespace fl::winrt {
std::u16string hstring_str(HSTRING s) {
    uint32_t len = 0;
    const char16_t* p = combase::c_WindowsGetStringRawBuffer(s, &len);
    return std::u16string(p, len);
}

HRESULT make_hstring(const char16_t* s, HSTRING* out) {
    return combase::c_WindowsCreateString(s, static_cast<uint32_t>(wstrlen(s)), out);
}
}  // namespace fl::winrt
