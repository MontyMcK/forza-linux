// msvcp110 (VS2012) iostream/locale object layouts shared by msvcp_locale.cpp
// and msvcp_iostream.cpp. The game inlines most of <xlocale>/<istream>/<ostream>
// and reads these fields directly, so every offset must match the real DLL
// (FINDINGS §4e has the evidence).
#pragma once

#include <cstddef>
#include <cstdint>

#include "common/winabi.h"

// The facet/stream structs mirror MSVC layouts via inheritance; offsetof on them is fine with clang.
#pragma clang diagnostic ignored "-Winvalid-offsetof"

namespace fl::crt::msvcp {

// Call vtable slot `slot` of an MSVC object (the game's or ours).
template <class R, class... A>
R vcall(const void* obj, int slot, A... args) {
    using Fn = R(WINAPI*)(const void*, A...);
    return (*reinterpret_cast<Fn* const*>(obj))[slot](obj, args...);
}

// _Yarn<char>: a malloc'd C string (freed with the CRT's free; ours is libc's).
struct Yarn {
    char* ptr;
    char nul;
};
void yarn_assign(Yarn* y, const char* s);
inline const char* yarn_c_str(const Yarn* y) { return y->ptr ? y->ptr : &y->nul; }

// locale::facet: {vft, 32-bit refcount}. vft slots: 0 scalar deleting dtor,
// 1 _Incref, 2 _Decref (returns this when the count drops to 0).
struct Facet {
    void* const* vt;
    uint32_t refs;
};
static_assert(sizeof(Facet) == 0x10);

// locale::_Locimp (0x38).
struct Locimp : Facet {
    Facet** facetvec;   // +0x10
    size_t facetcount;  // +0x18
    int32_t catmask;    // +0x20
    bool xparent;       // +0x24
    Yarn name;          // +0x28
};
static_assert(offsetof(Locimp, facetvec) == 0x10 && offsetof(Locimp, name) == 0x28 && sizeof(Locimp) == 0x38);

struct Locale {
    Locimp* ptr;
};

Locimp* locale_init(bool incref);  // locale::_Init
Locale* new_locale();              // new locale() (global locale, incref'd)
void delete_locale(Locale* loc);   // delete loc (decref + free)
void release_facet(Facet* f);      // delete f->_Decref()

// ctype_base::mask bits (= the CRT's _pctype bits).
enum : uint16_t {
    kUpper = 0x01,
    kLower = 0x02,
    kDigit = 0x04,
    kSpace = 0x08,
    kPunct = 0x10,
    kCntrl = 0x20,
    kBlank = 0x40,
    kXdigit = 0x80,
    kAlphaX = 0x100,
};
uint16_t ctype_mask(unsigned c);  // "C" locale classification (0 above 0xFF)

}  // namespace fl::crt::msvcp
