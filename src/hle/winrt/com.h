// COM / WinRT object authoring helpers.
//
// Interfaces are C++ structs with pure-virtual WINAPI methods and NO virtual
// destructor. Clang's Itanium vtable for such a struct is exactly a COM vtable
// (slot 0 = QueryInterface, ...). Implementations derive from one or more
// interfaces; QueryInterface hands out the right base pointer.
#pragma once

#include <atomic>
#include <cstring>
#include <string>

#include "common/winabi.h"

namespace fl::winrt {

constexpr GUID IID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr GUID IID_IInspectable = {0xAF86E2E0, 0xB12D, 0x4C6A, {0x9C, 0x5A, 0xD7, 0xAA, 0x65, 0x10, 0x1E, 0x90}};
constexpr GUID IID_IActivationFactory = {0x00000035, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr GUID IID_IAgileObject = {0x94EA2B94, 0xE9CC, 0x49E0, {0xC0, 0xFF, 0xEE, 0x64, 0xCA, 0x8F, 0x5B, 0x90}};
constexpr GUID IID_IMarshal = {0x00000003, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};

std::string guid_str(const GUID& g);

using HSTRING = struct HSTRING__*;

enum TrustLevel { BaseTrust = 0, PartialTrust = 1, FullTrust = 2 };

struct IUnknown {
    virtual HRESULT WINAPI QueryInterface(const GUID& iid, void** out) = 0;
    virtual ULONG WINAPI AddRef() = 0;
    virtual ULONG WINAPI Release() = 0;
};

struct IInspectable : IUnknown {
    virtual HRESULT WINAPI GetIids(ULONG* count, GUID** iids) = 0;
    virtual HRESULT WINAPI GetRuntimeClassName(HSTRING* name) = 0;
    virtual HRESULT WINAPI GetTrustLevel(TrustLevel* level) = 0;
};

struct IActivationFactory : IInspectable {
    virtual HRESULT WINAPI ActivateInstance(IInspectable** instance) = 0;
};

// Reference counting + IInspectable boilerplate. `Self` lists the interfaces
// it answers in its own QueryInterface override via query_one<>().
class RefCounted {
public:
    virtual ~RefCounted() = default;
    ULONG add_ref() { return ++refs_; }
    ULONG release() {
        ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }

private:
    std::atomic<ULONG> refs_{1};
};

}  // namespace fl::winrt

// Implements AddRef/Release/IInspectable methods in a class that derives from
// RefCounted and at least one IInspectable-based interface.
#define FL_WINRT_IUNKNOWN_IMPL                                                     \
    ULONG WINAPI AddRef() override { return add_ref(); }                           \
    ULONG WINAPI Release() override { return release(); }

#define FL_WINRT_IINSPECTABLE_IMPL(class_name_literal)                             \
    FL_WINRT_IUNKNOWN_IMPL                                                         \
    HRESULT WINAPI GetIids(ULONG* count, GUID** iids) override {                   \
        *count = 0;                                                                \
        *iids = nullptr;                                                           \
        return S_OK;                                                               \
    }                                                                              \
    HRESULT WINAPI GetRuntimeClassName(::fl::winrt::HSTRING* name) override {      \
        return ::fl::winrt::make_hstring(class_name_literal, name);                \
    }                                                                              \
    HRESULT WINAPI GetTrustLevel(::fl::winrt::TrustLevel* level) override {        \
        *level = ::fl::winrt::BaseTrust;                                           \
        return S_OK;                                                               \
    }

namespace fl::winrt {
HRESULT make_hstring(const char16_t* s, HSTRING* out);
std::u16string hstring_str(HSTRING s);  // copy of an HSTRING's text (null = empty)
}
