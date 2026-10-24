// msvcr110: C++ support: std::exception, std::bad_cast, type_info, RTTI casts.
//
// Object layouts (MSVC x64, VS2012):
//   std::exception { vftable*; const char* what; bool dofree; }   (24 bytes)
//   type_info      { vftable*; char* undecorated_cache; char decorated[]; }
// RTTI (all pointers are 32-bit RVAs from the image base):
//   CompleteObjectLocator { sig=1, offset, cdOffset, typeDesc, classDesc, self }
//   ClassHierarchyDescriptor { sig, attributes, numBases, baseArray }
//   BaseClassDescriptor { typeDesc, numContained, PMD{mdisp,pdisp,vdisp}, attributes, classDesc }

#include <cstdlib>
#include <cstring>
#include <string>

#include "common/log.h"
#include "common/winabi.h"
#include "hle/crt/crt_cxx.h"
#include "hle/registry.h"

namespace fl::crt {
namespace {

// ---------------------------------------------------------------- std::exception

using VFn = void*;

WINAPI void* exc_deleting_dtor(StdException* self, unsigned flags);
WINAPI const char* exc_what(const StdException* self);

// vtable[-1] would be the RTTI locator; nobody dynamic_casts a bare std::exception.
struct ExcVtable {
    void* rtti;
    VFn slots[2];
};
ExcVtable g_exception_vt = {nullptr, {reinterpret_cast<VFn>(&exc_deleting_dtor), reinterpret_cast<VFn>(&exc_what)}};
ExcVtable g_bad_cast_vt = {nullptr, {reinterpret_cast<VFn>(&exc_deleting_dtor), reinterpret_cast<VFn>(&exc_what)}};

void set_msg(StdException* e, const char* msg) {
    if (msg) {
        size_t n = strlen(msg) + 1;
        auto* copy = static_cast<char*>(malloc(n));
        memcpy(copy, msg, n);
        e->what = copy;
        e->dofree = true;
    } else {
        e->what = nullptr;
        e->dofree = false;
    }
}

WINAPI StdException* exc_ctor(StdException* self) {
    self->vft = g_exception_vt.slots;
    self->what = nullptr;
    self->dofree = false;
    return self;
}

WINAPI StdException* exc_ctor_msg(StdException* self, const char* const* msg) {
    self->vft = g_exception_vt.slots;
    set_msg(self, *msg);
    return self;
}

WINAPI StdException* exc_copy_ctor(StdException* self, const StdException* other) {
    self->vft = g_exception_vt.slots;
    if (other->dofree)
        set_msg(self, other->what);
    else {
        self->what = other->what;
        self->dofree = false;
    }
    return self;
}

WINAPI void exc_dtor(StdException* self) {
    if (self->dofree) free(const_cast<char*>(self->what));
    self->what = nullptr;
    self->dofree = false;
}

WINAPI const char* exc_what(const StdException* self) { return self->what ? self->what : "Unknown exception"; }

WINAPI void* exc_deleting_dtor(StdException* self, unsigned flags) {
    exc_dtor(self);
    if (flags & 1) {
        // The game allocated it with its own operator new; we can't free it correctly.
        FL_ERROR("deleting destructor of std::exception %p with delete flag: leaking", static_cast<void*>(self));
    }
    return self;
}

// std::bad_cast(const char*), copy ctor, dtor
WINAPI StdException* bad_cast_ctor_msg(StdException* self, const char* msg) {
    self->vft = g_bad_cast_vt.slots;
    set_msg(self, msg);
    return self;
}

WINAPI StdException* bad_cast_copy_ctor(StdException* self, const StdException* other) {
    exc_copy_ctor(self, other);
    self->vft = g_bad_cast_vt.slots;
    return self;
}

// ---------------------------------------------------------------- type_info

struct TypeInfo {
    void* vft;
    char* undecorated;
    char name[1];  // decorated, e.g. ".?AVFoo@@"
};

WINAPI bool ti_eq(const TypeInfo* a, const TypeInfo* b) { return a == b || strcmp(a->name + 1, b->name + 1) == 0; }
WINAPI bool ti_ne(const TypeInfo* a, const TypeInfo* b) { return !ti_eq(a, b); }
WINAPI bool ti_before(const TypeInfo* a, const TypeInfo* b) { return strcmp(a->name + 1, b->name + 1) < 0; }

// ".?AVFoo@ns@@" -> "class ns::Foo". Complex (template) names fall back to the decorated form.
std::string undecorate(const char* d) {
    std::string s = d;
    if (s.size() < 6 || s.compare(0, 3, ".?A") != 0 || s.compare(s.size() - 2, 2, "@@") != 0 ||
        s.find_first_of("?$", 3) != std::string::npos)
        return s;
    const char* kind = s[3] == 'V' ? "class " : s[3] == 'U' ? "struct " : s[3] == 'W' ? "enum " : "union ";
    std::string body = s.substr(4, s.size() - 6), out;
    size_t end = body.size();
    while (true) {
        size_t at = body.rfind('@', end - 1);
        std::string part = at == std::string::npos ? body.substr(0, end) : body.substr(at + 1, end - at - 1);
        out += (out.empty() ? "" : "::") + part;
        if (at == std::string::npos || at == 0) break;
        end = at;
    }
    return kind + out;
}

WINAPI const char* ti_name_internal(TypeInfo* self, void* node) {
    if (!self->undecorated) {
        std::string u = undecorate(self->name);
        auto* p = static_cast<char*>(malloc(u.size() + 1));
        memcpy(p, u.c_str(), u.size() + 1);
        // Benign race: worst case we leak one copy.
        if (!__sync_bool_compare_and_swap(&self->undecorated, nullptr, p)) free(p);
    }
    return self->undecorated;
}

WINAPI void ti_dtor_internal(TypeInfo* self) {
    free(self->undecorated);
    self->undecorated = nullptr;
}

// ---------------------------------------------------------------- RTTI

struct Col {
    uint32_t signature, offset, cd_offset, type_desc, class_desc, self;
};
struct Chd {
    uint32_t signature, attributes, num_bases, base_array;
};
struct Bcd {
    uint32_t type_desc, num_contained;
    int32_t mdisp, pdisp, vdisp;
    uint32_t attributes, class_desc;
};

const Col* locator_of(const void* obj) {
    auto* vtable = *reinterpret_cast<void* const* const*>(obj);
    return static_cast<const Col*>(vtable[-1]);
}

uint8_t* image_base_of(const Col* col) { return reinterpret_cast<uint8_t*>(const_cast<Col*>(col)) - col->self; }

uint8_t* complete_object(void* obj, const Col* col) {
    auto* p = static_cast<uint8_t*>(obj) - col->offset;
    if (col->cd_offset) p -= *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(obj) - col->cd_offset);
    return p;
}

int64_t this_offset(const Bcd* b, uint8_t* complete) {
    int64_t off = b->mdisp;
    if (b->pdisp >= 0) {
        auto* vbtable = *reinterpret_cast<int32_t**>(complete + b->pdisp);
        off += b->pdisp + vbtable[b->vdisp / 4];
    }
    return off;
}

bool same_type(const TypeInfo* a, const TypeInfo* b) { return a == b || strcmp(a->name, b->name) == 0; }

WINAPI const TypeInfo* c_RTtypeid(void* obj) {
    if (!obj) fl::fatal("typeid of a null pointer (would throw std::bad_typeid)");
    const Col* col = locator_of(obj);
    return reinterpret_cast<const TypeInfo*>(image_base_of(col) + col->type_desc);
}

WINAPI void* c_RTDynamicCast(void* obj, LONG vf_delta, void* src_type, void* dst_type, BOOL is_reference) {
    if (!obj) return nullptr;
    auto* adjusted = static_cast<uint8_t*>(obj) + vf_delta;
    const Col* col = locator_of(adjusted);
    uint8_t* base = image_base_of(col);
    uint8_t* complete = complete_object(adjusted, col);
    auto* chd = reinterpret_cast<const Chd*>(base + col->class_desc);
    auto* bases = reinterpret_cast<const uint32_t*>(base + chd->base_array);
    auto* target = static_cast<const TypeInfo*>(dst_type);
    for (uint32_t i = 0; i < chd->num_bases; i++) {
        auto* bcd = reinterpret_cast<const Bcd*>(base + bases[i]);
        auto* ti = reinterpret_cast<const TypeInfo*>(base + bcd->type_desc);
        if (same_type(ti, target)) return complete + this_offset(bcd, complete);
    }
    if (is_reference) fl::fatal("dynamic_cast<%s&> failed (would throw std::bad_cast)", target->name);
    return nullptr;
}

}  // namespace

void std_exception_init(StdException* e, const char* msg) {
    e->vft = g_exception_vt.slots;
    set_msg(e, msg);
}

HLE_EXPORT_AS(msvcr110, "??0exception@std@@QEAA@XZ", exc_ctor);
HLE_EXPORT_AS(msvcr110, "??0exception@std@@QEAA@AEBQEBD@Z", exc_ctor_msg);
HLE_EXPORT_AS(msvcr110, "??0exception@std@@QEAA@AEBV01@@Z", exc_copy_ctor);
HLE_EXPORT_AS(msvcr110, "??1exception@std@@UEAA@XZ", exc_dtor);
HLE_EXPORT_AS(msvcr110, "?what@exception@std@@UEBAPEBDXZ", exc_what);
HLE_EXPORT_AS(msvcr110, "??0bad_cast@std@@QEAA@PEBD@Z", bad_cast_ctor_msg);
HLE_EXPORT_AS(msvcr110, "??0bad_cast@std@@QEAA@AEBV01@@Z", bad_cast_copy_ctor);
HLE_EXPORT_AS(msvcr110, "??1bad_cast@std@@UEAA@XZ", exc_dtor);
HLE_EXPORT_AS(msvcr110, "??8type_info@@QEBA_NAEBV0@@Z", ti_eq);
HLE_EXPORT_AS(msvcr110, "??9type_info@@QEBA_NAEBV0@@Z", ti_ne);
HLE_EXPORT_AS(msvcr110, "?before@type_info@@QEBA_NAEBV1@@Z", ti_before);
HLE_EXPORT_AS(msvcr110, "?_name_internal_method@type_info@@QEBAPEBDPEAU__type_info_node@@@Z", ti_name_internal);
HLE_EXPORT_AS(msvcr110, "?_type_info_dtor_internal_method@type_info@@QEAAXXZ", ti_dtor_internal);
HLE_EXPORT(msvcr110, __RTtypeid, c_RTtypeid);
HLE_EXPORT(msvcr110, __RTDynamicCast, c_RTDynamicCast);

}  // namespace fl::crt
