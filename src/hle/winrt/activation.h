// WinRT activation: runtime class name -> activation factory.
#pragma once

#include "common/winabi.h"

namespace fl::winrt {

// Creates the class's activation factory and QIs it for `iid`.
using FactoryGetter = HRESULT (*)(const GUID& iid, void** out);

struct ClassRegistrar {
    ClassRegistrar(const char16_t* name, FactoryGetter getter);
};

HRESULT get_activation_factory(const char16_t* class_id, const GUID* iid, void** out);

constexpr HRESULT REGDB_E_CLASSNOTREG = static_cast<HRESULT>(0x80040154);

}  // namespace fl::winrt

#define FL_WINRT_CLASS_CAT2(a, b) a##b
#define FL_WINRT_CLASS_CAT(a, b) FL_WINRT_CLASS_CAT2(a, b)
#define WINRT_REGISTER_CLASS(name_literal, getter) \
    static ::fl::winrt::ClassRegistrar FL_WINRT_CLASS_CAT(fl_winrt_cls_, __COUNTER__)(name_literal, getter)
