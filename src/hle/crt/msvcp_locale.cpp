// msvcp110: std::locale, facets, ctype<char/wchar_t>, codecvt<wchar_t,char,int>,
// _Locinfo and the C collation helpers. We only ever provide the "C" locale.
//
// Layouts from the reference msvcp110.dll (FINDINGS §4e):
//   facet {vft, u32 refs}; _Locimp 0x38; locale {_Locimp*}; locale::id {size_t}
//   ctype<char> 0x30: +0x10 _Ctypevec {u32 page; const short* table @+0x18; int delfl; wchar_t* name}
//   ctype<wchar_t> 0x60: _Ctypevec @+0x10, _Cvtvec (0x2c) @+0x30
//   codecvt<wchar_t,char,int> 0x40: _Cvtvec @+0x10
//   _Locinfo 0x68: _Lockit @0, six _Yarns (0x10 each) from +8; names @+0x48/+0x58
// The game inlines use_facet and the ctype<char>::is() table lookup, so these
// must match exactly.

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "common/log.h"
#include "hle/crt/msvcp_ios.h"
#include "hle/registry.h"

namespace fl::crt::msvcp {

void yarn_assign(Yarn* y, const char* s) {
    if (y->ptr == s) return;
    free(y->ptr);
    y->ptr = nullptr;
    if (s) {
        size_t n = strlen(s) + 1;
        y->ptr = static_cast<char*>(malloc(n));
        memcpy(y->ptr, s, n);
    }
}

uint16_t ctype_mask(unsigned c) {
    if (c > 0xFF) return 0;
    if (c >= 'A' && c <= 'Z') return kUpper | kAlphaX | (c <= 'F' ? kXdigit : 0);
    if (c >= 'a' && c <= 'z') return kLower | kAlphaX | (c <= 'f' ? kXdigit : 0);
    if (c >= '0' && c <= '9') return kDigit | kXdigit;
    if (c == ' ') return kSpace | kBlank;
    if (c == '\t') return kCntrl | kSpace | kBlank;
    if (c >= 0x0A && c <= 0x0D) return kCntrl | kSpace;
    if (c < 0x20 || c == 0x7F) return kCntrl;
    if (c < 0x7F) return kPunct;
    return 0;  // "C" locale: nothing above 0x7F is classified
}

namespace {

// The locale lock (_Lockit kind 0, _LOCK_LOCALE). Recursive: _Getcat runs under it.
std::recursive_mutex& locale_lock() {
    static std::recursive_mutex m;
    return m;
}

// ---------------------------------------------------------------- facet basics

// __atomic builtins rather than std::atomic_ref (not in the PS5 SDK's libc++).
WINAPI void facet_incref(Facet* f) { __atomic_fetch_add(&f->refs, 1u, __ATOMIC_SEQ_CST); }
WINAPI Facet* facet_decref(Facet* f) { return __atomic_fetch_sub(&f->refs, 1u, __ATOMIC_SEQ_CST) == 1 ? f : nullptr; }

// Scalar deleting destructor of a facet that owns nothing.
WINAPI Facet* facet_delete(Facet* f, unsigned flags) {
    if (flags & 1) free(f);
    return f;
}

[[noreturn]] WINAPI void purecall_() { fl::fatal("msvcp110: pure virtual call (_Facet_base)"); }

WINAPI Facet* facet_ctor(Facet* f, size_t refs);

// ---------------------------------------------------------------- _Locimp

WINAPI Locimp* locimp_delete(Locimp* p, unsigned flags) {
    {
        std::lock_guard lock(locale_lock());
        for (size_t i = p->facetcount; i > 0;)
            if (Facet* f = p->facetvec[--i]) release_facet(f);
        free(p->facetvec);
    }
    free(p->name.ptr);
    if (flags & 1) free(p);
    return p;
}

void* g_locimp_vt[] = {(void*)&locimp_delete, (void*)&facet_incref, (void*)&facet_decref};

Locimp* new_locimp(bool transparent) {
    auto* p = static_cast<Locimp*>(calloc(1, sizeof(Locimp)));
    p->vt = g_locimp_vt;
    p->refs = 1;
    p->xparent = transparent;
    yarn_assign(&p->name, "*");
    return p;
}

Locimp* g_global = nullptr;  // global == classic: we only have "C"

// locale::id counter (locale::id::_Id_cnt)
std::atomic<size_t> g_id_cnt{0};

struct LocaleId {
    size_t id;
};

// ---------------------------------------------------------------- _Locinfo

struct YarnW {
    char16_t* ptr;
    char16_t nul;
};
struct Locinfo {
    int32_t lock;  // _Lockit
    Yarn days, months;
    YarnW wdays, wmonths;
    Yarn oldlocname, newlocname;
};
static_assert(offsetof(Locinfo, days) == 8 && offsetof(Locinfo, oldlocname) == 0x48 &&
              offsetof(Locinfo, newlocname) == 0x58 && sizeof(Locinfo) == 0x68);

WINAPI Locinfo* locinfo_ctor(Locinfo* li, const char* name) {
    locale_lock().lock();
    memset(li, 0, sizeof(*li));
    if (!name) fl::fatal("msvcp110: _Locinfo(nullptr) (bad locale name)");
    // Only the "C" locale exists; any other name is reported as unknown ("*"),
    // like setlocale() failing.
    yarn_assign(&li->oldlocname, "C");
    yarn_assign(&li->newlocname, (!*name || !strcmp(name, "C")) ? "C" : "*");
    return li;
}

WINAPI void locinfo_dtor(Locinfo* li) {
    free(li->days.ptr);
    free(li->months.ptr);
    free(li->wdays.ptr);
    free(li->wmonths.ptr);
    free(li->oldlocname.ptr);
    free(li->newlocname.ptr);
    memset(&li->days, 0, sizeof(*li) - offsetof(Locinfo, days));
    locale_lock().unlock();
}

WINAPI Yarn* yarn_assign_export(Yarn* y, const char* s) {
    yarn_assign(y, s);
    return y;
}

// ---------------------------------------------------------------- ctype<char>

struct Ctypevec {
    uint32_t page;
    const short* table;
    int32_t delfl;
    char16_t* name;
};
static_assert(sizeof(Ctypevec) == 0x20);

struct Cvtvec {
    uint32_t page;
    uint32_t mbcurmax;
    int32_t isclocale;
    uint8_t isleadbyte[32];
};
static_assert(sizeof(Cvtvec) == 0x2c);

const short* c_table() {
    static short t[256];
    static bool init = [] {
        for (unsigned c = 0; c < 256; c++) t[c] = static_cast<short>(ctype_mask(c));
        return true;
    }();
    (void)init;
    return t;
}

Ctypevec c_ctypevec() { return {0, c_table(), 0, nullptr}; }
Cvtvec c_cvtvec() { return {0, 1, 1, {}}; }

struct CtypeChar : Facet {
    Ctypevec ctype;  // +0x10
};
static_assert(offsetof(CtypeChar, ctype) == 0x10 && offsetof(CtypeChar, ctype.table) == 0x18 &&
              sizeof(CtypeChar) == 0x30);

char lower_c(char c) { return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; }
char upper_c(char c) { return (c >= 'a' && c <= 'z') ? char(c - 32) : c; }

WINAPI const char* cc_tolower_r(const CtypeChar*, char* f, const char* l) {
    for (; f != l; ++f) *f = lower_c(*f);
    return f;
}
WINAPI char cc_tolower(const CtypeChar*, char c) { return lower_c(c); }
WINAPI const char* cc_toupper_r(const CtypeChar*, char* f, const char* l) {
    for (; f != l; ++f) *f = upper_c(*f);
    return f;
}
WINAPI char cc_toupper(const CtypeChar*, char c) { return upper_c(c); }
WINAPI const char* cc_widen_r(const CtypeChar*, const char* f, const char* l, char* d) {
    memcpy(d, f, l - f);
    return l;
}
WINAPI char cc_widen(const CtypeChar*, char c) { return c; }  // also do_narrow(char, char)
WINAPI const char* cc_narrow_r(const CtypeChar*, const char* f, const char* l, char, char* d) {
    memcpy(d, f, l - f);
    return l;
}

void* g_ctype_char_vt[] = {(void*)&facet_delete,  (void*)&facet_incref, (void*)&facet_decref, (void*)&cc_tolower_r,
                           (void*)&cc_tolower,    (void*)&cc_toupper_r, (void*)&cc_toupper,   (void*)&cc_widen_r,
                           (void*)&cc_widen,      (void*)&cc_narrow_r,  (void*)&cc_widen};

constexpr size_t kXCtype = 2;  // _X_CTYPE: the category ctype/codecvt report

WINAPI size_t ctype_char_getcat(const Facet** ppf, const Locale*) {
    if (ppf && !*ppf) {
        auto* f = static_cast<CtypeChar*>(calloc(1, sizeof(CtypeChar)));
        f->vt = g_ctype_char_vt;
        f->ctype = c_ctypevec();
        *ppf = f;
    }
    return kXCtype;
}

// ---------------------------------------------------------------- ctype<wchar_t>

struct CtypeW : Facet {
    Ctypevec ctype;  // +0x10
    Cvtvec cvt;      // +0x30
};
static_assert(offsetof(CtypeW, cvt) == 0x30 && sizeof(CtypeW) == 0x60);

char16_t lower_w(char16_t c) { return (c >= u'A' && c <= u'Z') ? char16_t(c + 32) : c; }
char16_t upper_w(char16_t c) { return (c >= u'a' && c <= u'z') ? char16_t(c - 32) : c; }

WINAPI const char16_t* cw_is_r(const CtypeW*, const char16_t* f, const char16_t* l, int16_t* dest) {
    for (; f != l; ++f) *dest++ = static_cast<int16_t>(ctype_mask(*f));
    return f;
}
WINAPI bool cw_is(const CtypeW*, int16_t mask, char16_t c) { return (ctype_mask(c) & uint16_t(mask)) != 0; }
WINAPI const char16_t* cw_scan_is(const CtypeW*, int16_t mask, const char16_t* f, const char16_t* l) {
    while (f != l && !(ctype_mask(*f) & uint16_t(mask))) ++f;
    return f;
}
WINAPI const char16_t* cw_scan_not(const CtypeW*, int16_t mask, const char16_t* f, const char16_t* l) {
    while (f != l && (ctype_mask(*f) & uint16_t(mask))) ++f;
    return f;
}
WINAPI const char16_t* cw_tolower_r(const CtypeW*, char16_t* f, const char16_t* l) {
    for (; f != l; ++f) *f = lower_w(*f);
    return f;
}
WINAPI char16_t cw_tolower(const CtypeW*, char16_t c) { return lower_w(c); }
WINAPI const char16_t* cw_toupper_r(const CtypeW*, char16_t* f, const char16_t* l) {
    for (; f != l; ++f) *f = upper_w(*f);
    return f;
}
WINAPI char16_t cw_toupper(const CtypeW*, char16_t c) { return upper_w(c); }
WINAPI const char* cw_widen_r(const CtypeW*, const char* f, const char* l, char16_t* d) {
    for (; f != l; ++f) *d++ = static_cast<unsigned char>(*f);
    return l;
}
WINAPI char16_t cw_widen(const CtypeW*, char c) { return static_cast<unsigned char>(c); }
WINAPI const char16_t* cw_narrow_r(const CtypeW*, const char16_t* f, const char16_t* l, char dflt, char* d) {
    for (; f != l; ++f) *d++ = *f < 0x100 ? char(*f) : dflt;
    return l;
}
WINAPI char cw_narrow(const CtypeW*, char16_t c, char dflt) { return c < 0x100 ? char(c) : dflt; }

void* g_ctype_w_vt[] = {(void*)&facet_delete, (void*)&facet_incref, (void*)&facet_decref, (void*)&cw_is_r,
                        (void*)&cw_is,        (void*)&cw_scan_is,   (void*)&cw_scan_not,  (void*)&cw_tolower_r,
                        (void*)&cw_tolower,   (void*)&cw_toupper_r, (void*)&cw_toupper,   (void*)&cw_widen_r,
                        (void*)&cw_widen,     (void*)&cw_narrow_r,  (void*)&cw_narrow};

WINAPI size_t ctype_w_getcat(const Facet** ppf, const Locale*) {
    if (ppf && !*ppf) {
        auto* f = static_cast<CtypeW*>(calloc(1, sizeof(CtypeW)));
        f->vt = g_ctype_w_vt;
        f->ctype = c_ctypevec();
        f->cvt = c_cvtvec();
        *ppf = f;
    }
    return kXCtype;
}

// The non-virtual wrappers the game imports: forward to the facet's vtable
// (the facet may be the game's own subclass).
WINAPI bool ctype_w_is(const Facet* f, int16_t mask, char16_t c) { return vcall<bool>(f, 4, mask, c); }
WINAPI char16_t ctype_w_tolower(const Facet* f, char16_t c) { return vcall<char16_t>(f, 8, c); }
WINAPI const char16_t* ctype_w_tolower_r(const Facet* f, char16_t* a, const char16_t* b) {
    return vcall<const char16_t*>(f, 7, a, b);
}
WINAPI char16_t ctype_w_widen(const Facet* f, char c) { return vcall<char16_t>(f, 12, c); }
WINAPI char ctype_c_tolower(const Facet* f, char c) { return vcall<char>(f, 4, c); }
WINAPI const char* ctype_c_tolower_r(const Facet* f, char* a, const char* b) { return vcall<const char*>(f, 3, a, b); }

// ---------------------------------------------------------------- codecvt<wchar_t, char, int>

struct Codecvt : Facet {
    Cvtvec cvt;  // +0x10
};
static_assert(sizeof(Codecvt) == 0x40);

enum { kOk = 0, kPartial = 1, kError = 2, kNoconv = 3 };

WINAPI bool cvt_always_noconv(const Codecvt*) { return false; }
WINAPI int cvt_max_length(const Codecvt*) { return 5; }  // the reference returns 5 (MB_LEN_MAX)
WINAPI int cvt_encoding(const Codecvt*) { return 0; }    // varying length
// "C" locale: bytes map 1:1 to code units 0..255.
WINAPI int cvt_in(const Codecvt*, int*, const char* f1, const char* l1, const char** m1, char16_t* f2, char16_t* l2,
                  char16_t** m2) {
    while (f1 != l1 && f2 != l2) *f2++ = static_cast<unsigned char>(*f1++);
    *m1 = f1;
    *m2 = f2;
    return f1 == l1 ? kOk : kPartial;
}
WINAPI int cvt_out(const Codecvt*, int*, const char16_t* f1, const char16_t* l1, const char16_t** m1, char* f2,
                   char* l2, char** m2) {
    int r = kOk;
    while (f1 != l1 && f2 != l2) {
        if (*f1 > 0xFF) {
            r = kError;
            break;
        }
        *f2++ = char(*f1++);
    }
    *m1 = f1;
    *m2 = f2;
    return r != kOk ? r : (f1 == l1 ? kOk : kPartial);
}
WINAPI int cvt_unshift(const Codecvt*, int*, char* f2, char*, char** m2) {
    *m2 = f2;
    return kNoconv;
}
WINAPI int cvt_length(const Codecvt*, int*, const char* f1, const char* l1, size_t count) {
    return static_cast<int>(std::min<size_t>(l1 - f1, count));
}

void* g_codecvt_vt[] = {(void*)&facet_delete,  (void*)&facet_incref,   (void*)&facet_decref,
                        (void*)&cvt_always_noconv, (void*)&cvt_max_length, (void*)&cvt_encoding,
                        (void*)&cvt_in,        (void*)&cvt_out,        (void*)&cvt_unshift,
                        (void*)&cvt_length};

WINAPI Codecvt* codecvt_ctor(Codecvt* c, size_t refs) {
    c->vt = g_codecvt_vt;
    c->refs = static_cast<uint32_t>(refs);
    c->cvt = c_cvtvec();
    return c;
}

WINAPI size_t codecvt_getcat(const Facet** ppf, const Locale*) {
    if (ppf && !*ppf) *ppf = codecvt_ctor(static_cast<Codecvt*>(calloc(1, sizeof(Codecvt))), 0);
    return kXCtype;
}

WINAPI int codecvt_out(const Facet* f, int* st, const char16_t* f1, const char16_t* l1, const char16_t** m1, char* f2,
                       char* l2, char** m2) {
    return vcall<int>(f, 7, st, f1, l1, m1, f2, l2, m2);
}

// Base-class vtables the game's inline destructors of derived facets store.
WINAPI int one_(const void*) { return 1; }
void* g_facet_base_vt[] = {(void*)&facet_delete, (void*)&purecall_, (void*)&purecall_};
void* g_facet_vt[] = {(void*)&facet_delete, (void*)&facet_incref, (void*)&facet_decref};
void* g_codecvt_base_vt[] = {(void*)&facet_delete, (void*)&facet_incref, (void*)&facet_decref,
                             (void*)&cvt_always_noconv, (void*)&one_, (void*)&one_};

WINAPI Facet* facet_ctor(Facet* f, size_t refs) {
    f->vt = g_facet_vt;
    f->refs = static_cast<uint32_t>(refs);
    return f;
}

// ---------------------------------------------------------------- locale statics

WINAPI Locimp* locale_init_export(bool incref) { return locale_init(incref); }

WINAPI Locimp* get_global_locale() { return locale_init(false); }

WINAPI void locimp_addfac(Locimp* p, Facet* f, size_t id) {
    std::lock_guard lock(locale_lock());
    if (p->facetcount <= id) {
        size_t count = std::max<size_t>(id + 1, 40);
        p->facetvec = static_cast<Facet**>(realloc(p->facetvec, count * sizeof(Facet*)));
        for (; p->facetcount < count; ++p->facetcount) p->facetvec[p->facetcount] = nullptr;
    }
    vcall<void>(f, 1);  // _Incref
    if (p->facetvec[id]) release_facet(p->facetvec[id]);
    p->facetvec[id] = f;
}

// _Locimp::_New_Locimp(const _Locimp&): a copy sharing (incref'd) facets. The
// classic locale's standard facets are created lazily by use_facet (_Getcat),
// so copying what is there is enough.
WINAPI Locimp* new_locimp_copy(const Locimp* src) {
    std::lock_guard lock(locale_lock());
    Locimp* p = new_locimp(src->xparent);
    p->catmask = src->catmask;
    yarn_assign(&p->name, yarn_c_str(&src->name));
    if (src->facetcount) {
        p->facetvec = static_cast<Facet**>(calloc(src->facetcount, sizeof(Facet*)));
        p->facetcount = src->facetcount;
        for (size_t i = 0; i < src->facetcount; i++)
            if ((p->facetvec[i] = src->facetvec[i])) vcall<void>(p->facetvec[i], 1);
    }
    return p;
}

WINAPI LocaleId* id_ctor(LocaleId* id, size_t v) {
    id->id = v;
    return id;
}

WINAPI size_t id_value(LocaleId* id) {
    if (id->id == 0) {
        std::lock_guard lock(locale_lock());
        if (id->id == 0) id->id = ++g_id_cnt;
    }
    return id->id;
}

LocaleId g_id_codecvt_w{0}, g_id_collate_c{0}, g_id_collate_w{0}, g_id_ctype_c{0}, g_id_ctype_w{0};

// ---------------------------------------------------------------- C collation (xstrcoll.c etc.)

struct Collvec {
    uint32_t page;
    char16_t* name;
};

WINAPI Collvec* getcoll(Collvec* out) {
    *out = {0, nullptr};
    return out;
}

template <class C>
int coll_compare(const C* f1, const C* l1, const C* f2, const C* l2) {
    size_t n1 = l1 - f1, n2 = l2 - f2, n = std::min(n1, n2);
    for (size_t i = 0; i < n; i++)
        if (f1[i] != f2[i]) return f1[i] < f2[i] ? -1 : 1;  // unsigned compare, like memcmp/wmemcmp
    return n1 == n2 ? 0 : (n1 < n2 ? -1 : 1);
}

WINAPI int strcoll_(const char* f1, const char* l1, const char* f2, const char* l2, const Collvec*) {
    return coll_compare(reinterpret_cast<const unsigned char*>(f1), reinterpret_cast<const unsigned char*>(l1),
                        reinterpret_cast<const unsigned char*>(f2), reinterpret_cast<const unsigned char*>(l2));
}
WINAPI int wcscoll_(const char16_t* f1, const char16_t* l1, const char16_t* f2, const char16_t* l2, const Collvec*) {
    return coll_compare(f1, l1, f2, l2);
}

template <class C>
size_t xfrm(C* f1, C* l1, const C* f2, const C* l2) {
    size_t n = l2 - f2;
    if (n <= size_t(l1 - f1)) memcpy(f1, f2, n * sizeof(C));
    return n;
}
WINAPI size_t strxfrm_(char* f1, char* l1, const char* f2, const char* l2, const Collvec*) {
    return xfrm(f1, l1, f2, l2);
}
WINAPI size_t wcsxfrm_(char16_t* f1, char16_t* l1, const char16_t* f2, const char16_t* l2, const Collvec*) {
    return xfrm(f1, l1, f2, l2);
}

}  // namespace

Locimp* locale_init(bool incref) {
    std::lock_guard lock(locale_lock());
    if (!g_global) {
        g_global = new_locimp(false);
        g_global->catmask = 0x3f;  // locale::all
        yarn_assign(&g_global->name, "C");
    }
    if (incref) facet_incref(g_global);
    return g_global;
}

Locale* new_locale() {
    auto* l = static_cast<Locale*>(malloc(sizeof(Locale)));
    l->ptr = locale_init(true);
    return l;
}

void release_facet(Facet* f) {
    if (Facet* dead = vcall<Facet*>(f, 2)) vcall<void*>(dead, 0, 1u);
}

void delete_locale(Locale* loc) {
    if (!loc) return;
    if (loc->ptr) release_facet(loc->ptr);
    free(loc);
}

HLE_EXPORT_AS(msvcp110, "??0facet@locale@std@@IEAA@_K@Z", facet_ctor);
HLE_EXPORT_AS(msvcp110, "?_Incref@facet@locale@std@@UEAAXXZ", facet_incref);
HLE_EXPORT_AS(msvcp110, "?_Decref@facet@locale@std@@UEAAPEAV_Facet_base@3@XZ", facet_decref);
HLE_EXPORT_AS(msvcp110, "??_7facet@locale@std@@6B@", g_facet_vt);
HLE_EXPORT_AS(msvcp110, "??_7_Facet_base@std@@6B@", g_facet_base_vt);
HLE_EXPORT_AS(msvcp110, "??_7codecvt_base@std@@6B@", g_codecvt_base_vt);
HLE_EXPORT_AS(msvcp110, "??_7?$codecvt@_WDH@std@@6B@", g_codecvt_vt);
HLE_EXPORT_AS(msvcp110, "??0id@locale@std@@QEAA@_K@Z", id_ctor);
HLE_EXPORT_AS(msvcp110, "??Bid@locale@std@@QEAA_KXZ", id_value);
HLE_EXPORT_AS(msvcp110, "?id@?$codecvt@_WDH@std@@2V0locale@2@A", g_id_codecvt_w);
HLE_EXPORT_AS(msvcp110, "?id@?$collate@D@std@@2V0locale@2@A", g_id_collate_c);
HLE_EXPORT_AS(msvcp110, "?id@?$collate@_W@std@@2V0locale@2@A", g_id_collate_w);
HLE_EXPORT_AS(msvcp110, "?id@?$ctype@D@std@@2V0locale@2@A", g_id_ctype_c);
HLE_EXPORT_AS(msvcp110, "?id@?$ctype@_W@std@@2V0locale@2@A", g_id_ctype_w);
HLE_EXPORT_AS(msvcp110, "?_Init@locale@std@@CAPEAV_Locimp@12@_N@Z", locale_init_export);
HLE_EXPORT_AS(msvcp110, "?_Getgloballocale@locale@std@@CAPEAV_Locimp@12@XZ", get_global_locale);
HLE_EXPORT_AS(msvcp110, "?_Locimp_Addfac@_Locimp@locale@std@@CAXPEAV123@PEAVfacet@23@_K@Z", locimp_addfac);
HLE_EXPORT_AS(msvcp110, "?_New_Locimp@_Locimp@locale@std@@CAPEAV123@AEBV123@@Z", new_locimp_copy);
HLE_EXPORT_AS(msvcp110, "??0_Locinfo@std@@QEAA@PEBD@Z", locinfo_ctor);
HLE_EXPORT_AS(msvcp110, "??1_Locinfo@std@@QEAA@XZ", locinfo_dtor);
HLE_EXPORT_AS(msvcp110, "??4?$_Yarn@D@std@@QEAAAEAV01@PEBD@Z", yarn_assign_export);
HLE_EXPORT_AS(msvcp110, "?_Getcat@?$ctype@D@std@@SA_KPEAPEBVfacet@locale@2@PEBV42@@Z", ctype_char_getcat);
HLE_EXPORT_AS(msvcp110, "?_Getcat@?$ctype@_W@std@@SA_KPEAPEBVfacet@locale@2@PEBV42@@Z", ctype_w_getcat);
HLE_EXPORT_AS(msvcp110, "?_Getcat@?$codecvt@_WDH@std@@SA_KPEAPEBVfacet@locale@2@PEBV42@@Z", codecvt_getcat);
HLE_EXPORT_AS(msvcp110, "??0?$codecvt@_WDH@std@@QEAA@_K@Z", codecvt_ctor);
HLE_EXPORT_AS(msvcp110, "?out@?$codecvt@_WDH@std@@QEBAHAEAHPEB_W1AEAPEB_WPEAD3AEAPEAD@Z", codecvt_out);
HLE_EXPORT_AS(msvcp110, "?is@?$ctype@_W@std@@QEBA_NF_W@Z", ctype_w_is);
HLE_EXPORT_AS(msvcp110, "?tolower@?$ctype@_W@std@@QEBA_W_W@Z", ctype_w_tolower);
HLE_EXPORT_AS(msvcp110, "?tolower@?$ctype@_W@std@@QEBAPEB_WPEA_WPEB_W@Z", ctype_w_tolower_r);
HLE_EXPORT_AS(msvcp110, "?widen@?$ctype@_W@std@@QEBA_WD@Z", ctype_w_widen);
HLE_EXPORT_AS(msvcp110, "?tolower@?$ctype@D@std@@QEBADD@Z", ctype_c_tolower);
HLE_EXPORT_AS(msvcp110, "?tolower@?$ctype@D@std@@QEBAPEBDPEADPEBD@Z", ctype_c_tolower_r);
HLE_EXPORT(msvcp110, _Getcoll, getcoll);
HLE_EXPORT(msvcp110, _Strcoll, strcoll_);
HLE_EXPORT(msvcp110, _Strxfrm, strxfrm_);
HLE_EXPORT(msvcp110, _Wcscoll, wcscoll_);
HLE_EXPORT(msvcp110, _Wcsxfrm, wcsxfrm_);

}  // namespace fl::crt::msvcp
