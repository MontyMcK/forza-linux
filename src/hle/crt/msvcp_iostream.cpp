// msvcp110: basic_streambuf, ios_base/basic_ios, basic_ostream/istream/iostream
// for char and wchar_t, std::cout, setw/setprecision.
//
// The game instantiates the derived streams itself (ostringstream, stringbuf...)
// and inlines most of the headers, so layouts follow the reference DLL
// exactly (FINDINGS §4e):
//   basic_streambuf 0x68: vft; Gfirst +8; Pfirst +0x10; IGfirst +0x18 (=&Gfirst);
//     IPfirst +0x20; Gnext +0x28; Pnext +0x30; IGnext +0x38; IPnext +0x40;
//     int Gcount +0x48; Pcount +0x4c; IGcount +0x50; IPcount +0x58; locale* +0x60
//   ios_base 0x48: vft; Stdstr +8; Mystate +0x10; Except +0x14; Fmtfl +0x18;
//     Prec +0x20; Wide +0x28; Arr +0x30; Calls +0x38; locale* Ploc +0x40
//   basic_ios 0x60: + Mystrbuf +0x48; Tiestr +0x50; Fillch +0x58
//   basic_ostream: vbptr +0, vtordisp +0xc, basic_ios vbase at +0x10 (vbtable {0,0x10})
//   basic_istream: vbptr +0, Chcount +8, vtordisp +0x14, vbase +0x18
//   basic_iostream: istream part {vbptr, Chcount}, ostream vbptr +0x10, vtordisp +0x1c, vbase +0x20
// Constructors take a hidden "most derived" int after the declared args; the
// virtual-base destructors' `this` points at the vbase (static offset).

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>

#include "common/log.h"
#include "hle/crt/msvcp_ios.h"
#include "hle/registry.h"

namespace fl::crt::msvcp {
namespace {

// ---------------------------------------------------------------- traits

template <class E>
struct Tr;
template <>
struct Tr<char> {
    using Int = int;
    static constexpr Int eof = -1;
    static Int to_int(char c) { return static_cast<unsigned char>(c); }
};
template <>
struct Tr<char16_t> {
    using Int = uint16_t;
    static constexpr Int eof = 0xFFFF;
    static Int to_int(char16_t c) { return c; }
};

// fmtflags / iostate
enum : int32_t {
    kSkipws = 0x1,
    kUnitbuf = 0x2,
    kUppercase = 0x4,
    kShowbase = 0x8,
    kShowpoint = 0x10,
    kShowpos = 0x20,
    kLeft = 0x40,
    kRight = 0x80,
    kInternal = 0x100,
    kDec = 0x200,
    kOct = 0x400,
    kHex = 0x800,
    kScientific = 0x1000,
    kFixed = 0x2000,
    kBoolalpha = 0x4000,
    kAdjustfield = kLeft | kRight | kInternal,
    kBasefield = kDec | kOct | kHex,
    kFloatfield = kScientific | kFixed,
};
enum : int32_t { kGood = 0, kEofbit = 1, kFailbit = 2, kBadbit = 4, kStatmask = 0x17 };

// ---------------------------------------------------------------- basic_streambuf

template <class E>
struct Streambuf {
    void* const* vt;
    E* gfirst;
    E* pfirst;
    E** igfirst;
    E** ipfirst;
    E* gnext;
    E* pnext;
    E** ignext;
    E** ipnext;
    int32_t gcount;
    int32_t pcount;
    int32_t* igcount;
    int32_t* ipcount;
    Locale* plocale;

    using Int = typename Tr<E>::Int;
    // vtable slots
    enum { kDtor, kLock, kUnlock, kOverflow, kPbackfail, kShowmanyc, kUnderflow, kUflow, kXsgetn, kXsputn,
           kSeekoff, kSeekpos, kSetbuf, kSync, kImbue };

    E* gptr() const { return *ignext; }
    E* pptr() const { return *ipnext; }
    int64_t gnavail() const { return *ignext ? *igcount : 0; }
    int64_t pnavail() const { return *ipnext ? *ipcount : 0; }
    E* gninc() {
        --*igcount;
        return (*ignext)++;
    }
    E* gnpreinc() {
        --*igcount;
        return ++(*ignext);
    }
    E* pninc() {
        --*ipcount;
        return (*ipnext)++;
    }
    void gbump(int n) {
        *igcount -= n;
        *ignext += n;
    }
    void pbump(int n) {
        *ipcount -= n;
        *ipnext += n;
    }
    Int overflow(Int c) { return vcall<Int>(this, kOverflow, c); }
    Int underflow() { return vcall<Int>(this, kUnderflow); }
    Int uflow() { return vcall<Int>(this, kUflow); }
    int sync() { return vcall<int>(this, kSync); }
    void lock() { vcall<void>(this, kLock); }
    void unlock() { vcall<void>(this, kUnlock); }

    Int sbumpc() { return gnavail() > 0 ? Tr<E>::to_int(*gninc()) : uflow(); }
    Int sgetc() { return gnavail() > 0 ? Tr<E>::to_int(*gptr()) : underflow(); }
    Int snextc() {
        if (gnavail() > 1) return Tr<E>::to_int(*gnpreinc());
        return sbumpc() == Tr<E>::eof ? Tr<E>::eof : sgetc();
    }
    Int sputc(E c) { return pnavail() > 0 ? Tr<E>::to_int(*pninc() = c) : overflow(Tr<E>::to_int(c)); }
    int64_t sputn(const E* p, int64_t n) { return vcall<int64_t>(this, kXsputn, p, n); }
};
static_assert(offsetof(Streambuf<char>, igfirst) == 0x18 && offsetof(Streambuf<char>, gcount) == 0x48 &&
              offsetof(Streambuf<char>, plocale) == 0x60 && sizeof(Streambuf<char>) == 0x68);

template <class E>
void sb_init_ptrs(Streambuf<E>* sb) {
    sb->igfirst = &sb->gfirst;
    sb->ipfirst = &sb->pfirst;
    sb->ignext = &sb->gnext;
    sb->ipnext = &sb->pnext;
    sb->igcount = &sb->gcount;
    sb->ipcount = &sb->pcount;
    sb->gfirst = sb->pfirst = sb->gnext = sb->pnext = nullptr;
    sb->gcount = sb->pcount = 0;
}

template <class E>
extern void* sb_vt[15];

template <class E>
WINAPI Streambuf<E>* sb_ctor(Streambuf<E>* sb) {
    sb->vt = sb_vt<E>;
    sb->plocale = new_locale();
    sb_init_ptrs(sb);
    return sb;
}

template <class E>
WINAPI void sb_dtor(Streambuf<E>* sb) {
    sb->vt = sb_vt<E>;
    delete_locale(sb->plocale);
    sb->plocale = nullptr;
}

template <class E>
WINAPI Streambuf<E>* sb_delete(Streambuf<E>* sb, unsigned flags) {
    sb_dtor(sb);
    if (flags & 1) free(sb);
    return sb;
}

template <class E>
WINAPI void sb_nop(Streambuf<E>*) {}  // _Lock, _Unlock
template <class E>
WINAPI typename Tr<E>::Int sb_eof1(Streambuf<E>*, typename Tr<E>::Int) {  // overflow, pbackfail
    return Tr<E>::eof;
}
template <class E>
WINAPI typename Tr<E>::Int sb_eof0(Streambuf<E>*) {  // underflow
    return Tr<E>::eof;
}
template <class E>
WINAPI int64_t sb_showmanyc(Streambuf<E>*) {
    return 0;
}
template <class E>
WINAPI typename Tr<E>::Int sb_uflow(Streambuf<E>* sb) {
    return sb->underflow() == Tr<E>::eof ? Tr<E>::eof : Tr<E>::to_int(*sb->gninc());
}
template <class E>
WINAPI int64_t sb_xsgetn(Streambuf<E>* sb, E* p, int64_t n) {
    int64_t copied = 0;
    while (n > 0) {
        int64_t size = sb->gnavail();
        if (size > 0) {
            size = std::min(size, n);
            memcpy(p, sb->gptr(), size * sizeof(E));
            p += size;
            copied += size;
            n -= size;
            sb->gbump(static_cast<int>(size));
        } else {
            auto meta = sb->uflow();
            if (meta == Tr<E>::eof) break;
            *p++ = static_cast<E>(meta);
            ++copied;
            --n;
        }
    }
    return copied;
}
template <class E>
WINAPI int64_t sb_xsputn(Streambuf<E>* sb, const E* p, int64_t n) {
    int64_t copied = 0;
    while (n > 0) {
        int64_t size = sb->pnavail();
        if (size > 0) {
            size = std::min(size, n);
            memcpy(sb->pptr(), p, size * sizeof(E));
            p += size;
            copied += size;
            n -= size;
            sb->pbump(static_cast<int>(size));
        } else if (sb->overflow(Tr<E>::to_int(*p)) == Tr<E>::eof) {
            break;
        } else {
            ++p;
            ++copied;
            --n;
        }
    }
    return copied;
}
// fpos<int> {streamoff _Myoff; fpos_t _Fpos; int _Mystate} (0x18), returned via hidden pointer.
struct Fpos {
    int64_t off;
    int64_t fpos;
    int32_t state;
};
template <class E>
WINAPI Fpos* sb_seekoff(Streambuf<E>*, Fpos* out, int64_t, int, int) {
    *out = {-1, 0, 0};
    return out;
}
template <class E>
WINAPI Fpos* sb_seekpos(Streambuf<E>*, Fpos* out, const Fpos*, int) {
    *out = {-1, 0, 0};
    return out;
}
template <class E>
WINAPI Streambuf<E>* sb_setbuf(Streambuf<E>* sb, E*, int64_t) {
    return sb;
}
template <class E>
WINAPI int sb_sync(Streambuf<E>*) {
    return 0;
}
template <class E>
WINAPI void sb_imbue(Streambuf<E>*, const Locale*) {}

template <class E>
void* sb_vt[15] = {(void*)&sb_delete<E>, (void*)&sb_nop<E>,       (void*)&sb_nop<E>,   (void*)&sb_eof1<E>,
                   (void*)&sb_eof1<E>,   (void*)&sb_showmanyc<E>, (void*)&sb_eof0<E>,  (void*)&sb_uflow<E>,
                   (void*)&sb_xsgetn<E>, (void*)&sb_xsputn<E>,    (void*)&sb_seekoff<E>, (void*)&sb_seekpos<E>,
                   (void*)&sb_setbuf<E>, (void*)&sb_sync<E>,      (void*)&sb_imbue<E>};

// non-virtual members
template <class E>
WINAPI E* sb_eback(const Streambuf<E>* sb) {
    return *sb->igfirst;
}
template <class E>
WINAPI E* sb_egptr(const Streambuf<E>* sb) {
    return *sb->ignext + *sb->igcount;
}
template <class E>
WINAPI E* sb_pptr(const Streambuf<E>* sb) {
    return *sb->ipnext;
}
template <class E>
WINAPI E* sb_epptr(const Streambuf<E>* sb) {
    return *sb->ipnext + *sb->ipcount;
}
template <class E>
WINAPI E* sb_pninc(Streambuf<E>* sb) {
    return sb->pninc();
}
template <class E>
WINAPI void sb_setg(Streambuf<E>* sb, E* first, E* next, E* last) {
    *sb->igfirst = first;
    *sb->ignext = next;
    *sb->igcount = static_cast<int32_t>(last - next);
}
template <class E>
WINAPI void sb_setp(Streambuf<E>* sb, E* first, E* last) {
    *sb->ipfirst = first;
    *sb->ipnext = first;
    *sb->ipcount = static_cast<int32_t>(last - first);
}
template <class E>
WINAPI typename Tr<E>::Int sb_sbumpc(Streambuf<E>* sb) {
    return sb->sbumpc();
}
template <class E>
WINAPI typename Tr<E>::Int sb_sgetc(Streambuf<E>* sb) {
    return sb->sgetc();
}
template <class E>
WINAPI typename Tr<E>::Int sb_snextc(Streambuf<E>* sb) {
    return sb->snextc();
}
template <class E>
WINAPI typename Tr<E>::Int sb_sputc(Streambuf<E>* sb, E c) {
    return sb->sputc(c);
}
template <class E>
WINAPI int64_t sb_sputn(Streambuf<E>* sb, const E* p, int64_t n) {
    return sb->sputn(p, n);
}

// ---------------------------------------------------------------- ios_base / basic_ios

struct IosBase {
    void* const* vt;
    size_t stdstr;
    int32_t state;
    int32_t except;
    int32_t fmtfl;
    int64_t prec;
    int64_t wide;
    void* arr;
    void* calls;
    Locale* ploc;
};
static_assert(offsetof(IosBase, state) == 0x10 && offsetof(IosBase, fmtfl) == 0x18 && offsetof(IosBase, prec) == 0x20 &&
              offsetof(IosBase, ploc) == 0x40 && sizeof(IosBase) == 0x48);

template <class E>
struct BasicIos : IosBase {
    Streambuf<E>* strbuf;
    void* tiestr;  // basic_ostream<E>*
    E fillch;
};
static_assert(offsetof(BasicIos<char>, strbuf) == 0x48 && offsetof(BasicIos<char>, fillch) == 0x58 &&
              sizeof(BasicIos<char>) == 0x60);

void ios_clear(IosBase* ios, int32_t st) {
    ios->state = st & kStatmask;
    if (ios->state & ios->except)
        fl::fatal("msvcp110: ios_base::failure (state 0x%x, exceptions 0x%x) - exceptions not implemented", ios->state,
                  ios->except);
}

template <class E>
void bios_clear(BasicIos<E>* ios, int32_t st) {
    ios_clear(ios, ios->strbuf ? st : (st | kBadbit));
}

template <class E>
void bios_setstate(BasicIos<E>* ios, int32_t st) {
    if (st != kGood) bios_clear(ios, ios->state | st);
}

WINAPI void ios_base_dtor(IosBase* ios) {
    // STUB: iword/pword/register_callback are not exported by us, so _Arr and
    // _Calls stay empty; _Stdstr counting (cout etc.) is not tracked.
    if (ios->arr || ios->calls) FL_ERROR("msvcp110: ios_base with iword/callback storage destroyed (leaked)");
    delete_locale(ios->ploc);
    ios->ploc = nullptr;
}

extern void* g_ios_base_vt[1];
template <class E>
extern void* bios_vt[3];

WINAPI IosBase* ios_base_delete(IosBase* ios, unsigned flags) {
    ios->vt = g_ios_base_vt;
    ios_base_dtor(ios);
    if (flags & 1) free(ios);
    return ios;
}
void* g_ios_base_vt[1] = {(void*)&ios_base_delete};

template <class E>
WINAPI BasicIos<E>* bios_ctor(BasicIos<E>* ios) {
    ios->vt = bios_vt<E>;
    return ios;
}
template <class E>
WINAPI void bios_dtor(BasicIos<E>* ios) {
    ios->vt = g_ios_base_vt;
    ios_base_dtor(ios);
}
template <class E>
WINAPI BasicIos<E>* bios_delete(BasicIos<E>* ios, unsigned flags) {
    bios_dtor(ios);
    if (flags & 1) free(ios);
    return ios;
}
WINAPI void vt_nop(void*) {}  // _Add_vtordisp1/2

template <class E>
void* bios_vt[3] = {(void*)&bios_delete<E>, (void*)&vt_nop, (void*)&vt_nop};

// basic_ios::init (ios_base::_Init + stream buffer)
template <class E>
void bios_init(BasicIos<E>* ios, Streambuf<E>* sb, bool) {
    ios->ploc = nullptr;
    ios->stdstr = 0;
    ios->except = kGood;
    ios->fmtfl = kSkipws | kDec;
    ios->prec = 6;
    ios->wide = 0;
    ios->arr = nullptr;
    ios->calls = nullptr;
    ios->state = kGood;
    ios->ploc = new_locale();
    ios->strbuf = sb;
    ios->tiestr = nullptr;
    ios->fillch = E(' ');
    if (!sb) bios_setstate(ios, kBadbit);
}

template <class E>
WINAPI void bios_clear_export(BasicIos<E>* ios, int32_t st, bool reraise) {
    if (reraise && (st & ios->except)) fl::fatal("msvcp110: basic_ios::clear reraise");
    bios_clear(ios, st);
}
template <class E>
WINAPI void bios_setstate_export(BasicIos<E>* ios, int32_t st, bool) {
    bios_setstate(ios, st);
}
template <class E>
WINAPI Streambuf<E>* bios_rdbuf(const BasicIos<E>* ios) {
    return ios->strbuf;
}
WINAPI char16_t bios_widen_w(const BasicIos<char16_t>*, char c) { return static_cast<unsigned char>(c); }

WINAPI int32_t ios_flags(const IosBase* ios) { return ios->fmtfl; }
WINAPI int64_t ios_width_get(const IosBase* ios) { return ios->wide; }
WINAPI int64_t ios_width_set(IosBase* ios, int64_t w) {
    int64_t old = ios->wide;
    ios->wide = w;
    return old;
}
WINAPI Locale* ios_getloc(const IosBase* ios, Locale* out) {
    out->ptr = ios->ploc->ptr;
    vcall<void>(out->ptr, 1);  // _Incref
    return out;
}

// ---------------------------------------------------------------- streams (virtual base helpers)

// The basic_ios virtual base of a stream whose vbptr is at `s`.
template <class E>
BasicIos<E>* vbase(void* s) {
    auto* vbtable = *static_cast<int32_t**>(s);
    return reinterpret_cast<BasicIos<E>*>(static_cast<char*>(s) + vbtable[1]);
}

// Set the vbase's vftable and the vtordisp in front of it for a class whose
// static vbase offset is `static_off`.
void set_vbase_vt(void* s, void* const* vt, int32_t static_off) {
    auto* vbtable = *static_cast<int32_t**>(s);
    int32_t off = vbtable[1];
    *reinterpret_cast<void* const**>(static_cast<char*>(s) + off) = vt;
    *reinterpret_cast<int32_t*>(static_cast<char*>(s) + off - 4) = off - static_off;
}

int32_t g_ostream_vbtable[2] = {0, 0x10};
int32_t g_istream_vbtable[2] = {0, 0x18};
int32_t g_iostream_vbtable_i[2] = {0, 0x20};
int32_t g_iostream_vbtable_o[2] = {0, 0x10};

template <class E>
extern void* ostream_vt[3];
template <class E>
extern void* istream_vt[3];
template <class E>
extern void* iostream_vt[3];

// ---------------------------------------------------------------- basic_ostream

template <class E>
WINAPI void* ostream_flush(void* os);

template <class E>
WINAPI void ostream_osfx(void* os) {
    BasicIos<E>* ios = vbase<E>(os);
    if (ios->state == kGood && (ios->fmtfl & kUnitbuf))
        if (ios->strbuf->sync() == -1) bios_setstate(ios, kBadbit);
}

template <class E>
struct OSentry {
    void* os;
    BasicIos<E>* ios;
    bool ok;
    explicit OSentry(void* s) : os(s), ios(vbase<E>(s)) {
        if (ios->strbuf) ios->strbuf->lock();
        if (ios->state == kGood && ios->tiestr) ostream_flush<E>(ios->tiestr);
        ok = ios->state == kGood;
    }
    ~OSentry() {
        ostream_osfx<E>(os);
        if (ios->strbuf) ios->strbuf->unlock();
    }
};

template <class E>
WINAPI void* ostream_ctor(void* os, Streambuf<E>* sb, bool isstd, int most_derived) {
    if (most_derived) {
        *static_cast<int32_t**>(os) = g_ostream_vbtable;
        *reinterpret_cast<void**>(static_cast<char*>(os) + 0x10) = bios_vt<E>;
    }
    set_vbase_vt(os, ostream_vt<E>, 0x10);
    bios_init(vbase<E>(os), sb, isstd);
    return os;
}

// `this` = the vbase at its static offset (0x10).
template <class E>
WINAPI void ostream_dtor(char* self) {
    set_vbase_vt(self - 0x10, ostream_vt<E>, 0x10);
}

// vftable slot 0 for a complete basic_ostream: vtordisp thunk + vbase dtor + free.
template <class E>
WINAPI void* ostream_delete(char* self, unsigned flags) {
    self -= *reinterpret_cast<int32_t*>(self - 4);
    char* full = self - 0x10;
    ostream_dtor<E>(self);
    bios_dtor(reinterpret_cast<BasicIos<E>*>(self));
    if (flags & 1) free(full);
    return full;
}

template <class E>
void* ostream_vt[3] = {(void*)&ostream_delete<E>, (void*)&vt_nop, (void*)&vt_nop};

template <class E>
WINAPI void* ostream_flush(void* os) {
    BasicIos<E>* ios = vbase<E>(os);
    if (ios->strbuf) {
        OSentry<E> ok(os);
        if (ok.ok && ios->strbuf->sync() == -1) bios_setstate(ios, kBadbit);
    }
    return os;
}

// Put a formatted field with fill/width/adjust (num_put's _Iput/_Fput). The
// first `prefix` chars (sign, 0x) go before internal fill. Returns false if
// the stream buffer refused a character.
template <class E>
bool put_field(BasicIos<E>* ios, const char* s, size_t n, size_t prefix) {
    Streambuf<E>* sb = ios->strbuf;
    bool ok = true;
    auto put = [&](E c) {
        if (ok && sb->sputc(c) == Tr<E>::eof) ok = false;
    };
    size_t fill = (ios->wide <= 0 || size_t(ios->wide) <= n) ? 0 : size_t(ios->wide) - n;
    int32_t adjust = ios->fmtfl & kAdjustfield;
    size_t i = 0;
    if (adjust != kLeft && adjust != kInternal) {
        for (; fill; --fill) put(ios->fillch);
    } else if (adjust == kInternal) {
        for (; i < prefix; i++) put(E(static_cast<unsigned char>(s[i])));
        for (; fill; --fill) put(ios->fillch);
    }
    for (; i < n; i++) put(E(static_cast<unsigned char>(s[i])));
    ios->wide = 0;
    for (; fill; --fill) put(ios->fillch);
    return ok;
}

size_t num_prefix(const char* s) {
    if (*s == '+' || *s == '-') return 1;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return 2;
    return 0;
}

// printf-format an integer per the stream flags (num_put::_Ifmt).
std::string fmt_int(int32_t fl, bool is_signed, bool wide64, int64_t sv, uint64_t uv) {
    char fmt[16], *p = fmt, buf[80];
    *p++ = '%';
    if (fl & kShowpos) *p++ = '+';
    if (fl & kShowbase) *p++ = '#';
    *p++ = 'l';
    *p++ = 'l';
    int32_t base = fl & kBasefield;
    *p++ = base == kOct ? 'o' : base != kHex ? (is_signed ? 'd' : 'u') : (fl & kUppercase) ? 'X' : 'x';
    *p = 0;
    (void)wide64;
    if (is_signed && base != kOct && base != kHex)
        snprintf(buf, sizeof buf, fmt, static_cast<long long>(sv));
    else
        snprintf(buf, sizeof buf, fmt, static_cast<unsigned long long>(uv));
    return buf;
}

std::string fmt_double(int32_t fl, int64_t prec, double v) {
    char fmt[16], *p = fmt;
    *p++ = '%';
    if (fl & kShowpos) *p++ = '+';
    if (fl & kShowpoint) *p++ = '#';
    *p++ = '.';
    *p++ = '*';
    int32_t ff = fl & kFloatfield;
    bool up = fl & kUppercase;
    *p++ = ff == kFixed ? 'f' : ff == kFloatfield ? (up ? 'A' : 'a') : ff == kScientific ? (up ? 'E' : 'e') : (up ? 'G' : 'g');
    *p = 0;
    if (prec <= 0 && ff != kFixed) prec = 6;
    prec = std::min<int64_t>(prec, 1000);
    int n = snprintf(nullptr, 0, fmt, int(prec), v);
    std::string s(n, '\0');
    snprintf(s.data(), n + 1, fmt, int(prec), v);
    return s;
}

template <class E>
void* ostream_put_str(void* os, const std::string& s) {
    BasicIos<E>* ios = vbase<E>(os);
    int32_t st = kGood;
    {
        OSentry<E> ok(os);
        if (ok.ok && !put_field(ios, s.data(), s.size(), num_prefix(s.c_str()))) st |= kBadbit;
    }
    bios_setstate(ios, st);
    return os;
}

// operator<< for integers. `Narrow` is the type MSVC widens to before num_put
// (long for short/int, unsigned long...): hex/oct output of a negative short
// or int prints the bits of the original width.
template <class E, class T>
WINAPI void* ostream_put_int(void* os, T v) {
    int32_t fl = vbase<E>(os)->fmtfl;
    using U = std::make_unsigned_t<T>;
    std::string s = std::is_signed_v<T> ? fmt_int(fl, true, sizeof(T) == 8, int64_t(v), uint64_t(U(v)))
                                        : fmt_int(fl, false, sizeof(T) == 8, 0, uint64_t(v));
    return ostream_put_str<E>(os, s);
}

template <class E>
WINAPI void* ostream_put_double(void* os, double v) {
    BasicIos<E>* ios = vbase<E>(os);
    return ostream_put_str<E>(os, fmt_double(ios->fmtfl, ios->prec, v));
}
template <class E>
WINAPI void* ostream_put_float(void* os, float v) {
    return ostream_put_double<E>(os, v);
}
template <class E>
WINAPI void* ostream_put_bool(void* os, bool v) {
    BasicIos<E>* ios = vbase<E>(os);
    if (!(ios->fmtfl & kBoolalpha)) return ostream_put_int<E, int32_t>(os, v ? 1 : 0);
    int32_t st = kGood;
    {
        OSentry<E> ok(os);
        const char* s = v ? "true" : "false";
        if (ok.ok && !put_field(ios, s, strlen(s), 0)) st |= kBadbit;
    }
    bios_setstate(ios, st);
    return os;
}

using IosManip = IosBase*(WINAPI*)(IosBase*);
WINAPI void* ostream_put_manip(void* os, IosManip fn) {
    fn(vbase<char>(os));
    return os;
}

// ---------------------------------------------------------------- basic_istream

template <class E>
WINAPI bool istream_ipfx(void* is, bool noskip) {
    BasicIos<E>* ios = vbase<E>(is);
    if (ios->state == kGood) {
        if (ios->tiestr) ostream_flush<E>(ios->tiestr);
        if (!noskip && (ios->fmtfl & kSkipws)) {
            auto meta = ios->strbuf->sgetc();
            for (;; meta = ios->strbuf->snextc()) {
                if (meta == Tr<E>::eof) {
                    bios_setstate(ios, kEofbit);
                    break;
                }
                if (!(ctype_mask(meta) & kSpace)) break;
            }
        }
        if (ios->state == kGood) return true;
    }
    bios_setstate(ios, kFailbit);
    return false;
}

template <class E>
struct ISentry {
    BasicIos<E>* ios;
    bool ok;
    ISentry(void* is, bool noskip) : ios(vbase<E>(is)) {
        if (ios->strbuf) ios->strbuf->lock();
        ok = istream_ipfx<E>(is, noskip);
    }
    ~ISentry() {
        if (ios->strbuf) ios->strbuf->unlock();
    }
};

template <class E>
WINAPI void* istream_ctor(void* is, Streambuf<E>* sb, bool isstd, int most_derived) {
    if (most_derived) {
        *static_cast<int32_t**>(is) = g_istream_vbtable;
        *reinterpret_cast<void**>(static_cast<char*>(is) + 0x18) = bios_vt<E>;
    }
    set_vbase_vt(is, istream_vt<E>, 0x18);
    *reinterpret_cast<int64_t*>(static_cast<char*>(is) + 8) = 0;  // _Chcount
    bios_init(vbase<E>(is), sb, isstd);
    return is;
}
template <class E>
WINAPI void istream_dtor(char* self) {
    set_vbase_vt(self - 0x18, istream_vt<E>, 0x18);
}
template <class E>
WINAPI void* istream_delete(char* self, unsigned flags) {
    self -= *reinterpret_cast<int32_t*>(self - 4);
    char* full = self - 0x18;
    istream_dtor<E>(self);
    bios_dtor(reinterpret_cast<BasicIos<E>*>(self));
    if (flags & 1) free(full);
    return full;
}
template <class E>
void* istream_vt[3] = {(void*)&istream_delete<E>, (void*)&vt_nop, (void*)&vt_nop};

// Scan a numeric field from the stream buffer into `out` (num_get's _Getifld /
// _Getffld, "C" locale). Sets eofbit in *st when input ran out.
template <class E>
std::string scan_number(BasicIos<E>* ios, bool is_float, int* base, int32_t* st) {
    Streambuf<E>* sb = ios->strbuf;
    std::string s;
    auto meta = sb->sgetc();
    auto ch = [&]() -> int { return meta == Tr<E>::eof ? -1 : (meta < 0x80 ? int(meta) : 0x80); };
    auto next = [&] { meta = sb->snextc(); };
    if (ch() == '+' || ch() == '-') {
        s += char(ch());
        next();
    }
    if (!is_float) {
        int32_t bf = ios->fmtfl & kBasefield;
        *base = bf == kOct ? 8 : bf == kHex ? 16 : bf == 0 ? 0 : 10;
        if (ch() == '0') {
            s += '0';
            next();
            if ((ch() == 'x' || ch() == 'X') && (*base == 0 || *base == 16)) {
                *base = 16;
                s.pop_back();
                next();
            } else if (*base == 0) {
                *base = 8;
            }
        }
        for (;;) {
            int c = ch();
            bool digit = (c >= '0' && c <= '9' && c - '0' < (*base ? *base : 10)) ||
                         (*base == 16 && ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')));
            if (!digit || s.size() > 400) break;
            s += char(c);
            next();
        }
    } else {
        while (ch() >= '0' && ch() <= '9' && s.size() < 400) {
            s += char(ch());
            next();
        }
        if (ch() == '.') {
            s += '.';
            next();
            while (ch() >= '0' && ch() <= '9' && s.size() < 800) {
                s += char(ch());
                next();
            }
        }
        if (ch() == 'e' || ch() == 'E') {
            s += 'e';
            next();
            if (ch() == '+' || ch() == '-') {
                s += char(ch());
                next();
            }
            while (ch() >= '0' && ch() <= '9' && s.size() < 900) {
                s += char(ch());
                next();
            }
        }
    }
    if (meta == Tr<E>::eof) *st |= kEofbit;
    return s;
}

// operator>> for integer type T.
template <class E, class T>
WINAPI void* istream_get_int(void* is, T* out) {
    BasicIos<E>* ios = vbase<E>(is);
    int32_t st = kGood;
    {
        ISentry<E> ok(is, false);
        if (ok.ok) {
            int base = 10;
            std::string s = scan_number(ios, false, &base, &st);
            char* end = nullptr;
            errno = 0;
            bool fail;
            if constexpr (std::is_signed_v<T>) {
                long long v = strtoll(s.c_str(), &end, base);
                fail = end == s.c_str() || *end || errno || v < std::numeric_limits<T>::min() ||
                       v > std::numeric_limits<T>::max();
                if (!fail) *out = static_cast<T>(v);
            } else {
                unsigned long long v = strtoull(s.c_str(), &end, base);
                // strtoull wraps "-n" like MSVC's _Stoulx; a negated value must still fit.
                bool neg = !s.empty() && s[0] == '-';
                fail = end == s.c_str() || *end || errno ||
                       (!neg && v > std::numeric_limits<T>::max()) ||
                       (neg && (0 - v) > uint64_t(std::numeric_limits<T>::max()) + 1);
                if (!fail) *out = static_cast<T>(v);
            }
            if (fail) st |= kFailbit;
        }
    }
    bios_setstate(ios, st);
    return is;
}

template <class E, class T>
WINAPI void* istream_get_float(void* is, T* out) {
    BasicIos<E>* ios = vbase<E>(is);
    int32_t st = kGood;
    {
        ISentry<E> ok(is, false);
        if (ok.ok) {
            int base = 10;
            std::string s = scan_number(ios, true, &base, &st);
            char* end = nullptr;
            errno = 0;
            double v = strtod(s.c_str(), &end);
            if (end == s.c_str() || *end || (errno == ERANGE && std::isinf(v)))
                st |= kFailbit;
            else
                *out = static_cast<T>(v);
        }
    }
    bios_setstate(ios, st);
    return is;
}

// ---------------------------------------------------------------- basic_iostream

template <class E>
WINAPI void* iostream_ctor(char* s, Streambuf<E>* sb, int most_derived) {
    if (most_derived) {
        *reinterpret_cast<int32_t**>(s) = g_iostream_vbtable_i;
        *reinterpret_cast<int32_t**>(s + 0x10) = g_iostream_vbtable_o;
        *reinterpret_cast<void**>(s + 0x20) = bios_vt<E>;
    }
    istream_ctor<E>(s, sb, false, 0);
    set_vbase_vt(s + 0x10, ostream_vt<E>, 0x10);  // basic_ostream(_Noinit): no init
    set_vbase_vt(s, iostream_vt<E>, 0x20);
    return s;
}
// `this` = vbase at static offset 0x20.
template <class E>
WINAPI void iostream_dtor(char* self) {
    char* full = self - 0x20;
    set_vbase_vt(full, iostream_vt<E>, 0x20);
    set_vbase_vt(full + 0x10, ostream_vt<E>, 0x10);
    set_vbase_vt(full, istream_vt<E>, 0x18);
}
template <class E>
WINAPI void* iostream_delete(char* self, unsigned flags) {
    self -= *reinterpret_cast<int32_t*>(self - 4);
    char* full = self - 0x20;
    iostream_dtor<E>(self);
    bios_dtor(reinterpret_cast<BasicIos<E>*>(self));
    if (flags & 1) free(full);
    return full;
}
template <class E>
void* iostream_vt[3] = {(void*)&iostream_delete<E>, (void*)&vt_nop, (void*)&vt_nop};

// ---------------------------------------------------------------- manipulators

struct Smanip {
    void(WINAPI* fn)(IosBase*, int64_t);
    int64_t arg;
};
WINAPI void set_width(IosBase* ios, int64_t w) { ios->wide = w; }
WINAPI void set_precision(IosBase* ios, int64_t p) { ios->prec = p; }
WINAPI Smanip* setw_(Smanip* out, int64_t w) {
    *out = {&set_width, w};
    return out;
}
WINAPI Smanip* setprecision_(Smanip* out, int64_t p) {
    *out = {&set_precision, p};
    return out;
}

char g_placeholder1, g_placeholder2;  // std::placeholders::_1/_2 (empty objects)

// ---------------------------------------------------------------- std::cout

WINAPI int cout_overflow(Streambuf<char>*, int c) {
    if (c == -1) return 0;
    fputc(c, stdout);
    return c;
}
WINAPI int64_t cout_xsputn(Streambuf<char>*, const char* p, int64_t n) { return int64_t(fwrite(p, 1, n, stdout)); }
WINAPI int cout_sync(Streambuf<char>*) { return fflush(stdout) == 0 ? 0 : -1; }

void* g_cout_sb_vt[15];

alignas(16) char g_cout[0x70];
alignas(16) Streambuf<char> g_cout_sb;

[[maybe_unused]] bool g_cout_init = [] {
    // Built here, not copied from sb_vt<char>: template variables have no
    // guaranteed dynamic-initialisation order relative to this one.
    using C = char;
    void* base[15] = {(void*)&sb_delete<C>, (void*)&sb_nop<C>,     (void*)&sb_nop<C>,     (void*)&sb_eof1<C>,
                      (void*)&sb_eof1<C>,   (void*)&sb_showmanyc<C>, (void*)&sb_eof0<C>, (void*)&sb_uflow<C>,
                      (void*)&sb_xsgetn<C>, (void*)&sb_xsputn<C>,  (void*)&sb_seekoff<C>, (void*)&sb_seekpos<C>,
                      (void*)&sb_setbuf<C>, (void*)&sb_sync<C>,    (void*)&sb_imbue<C>};
    std::copy(std::begin(base), std::end(base), g_cout_sb_vt);
    g_cout_sb_vt[Streambuf<char>::kOverflow] = (void*)&cout_overflow;
    g_cout_sb_vt[Streambuf<char>::kXsputn] = (void*)&cout_xsputn;
    g_cout_sb_vt[Streambuf<char>::kSync] = (void*)&cout_sync;
    sb_ctor(&g_cout_sb);
    g_cout_sb.vt = g_cout_sb_vt;
    ostream_ctor<char>(g_cout, &g_cout_sb, true, 1);
    return true;
}();

}  // namespace

using SbC = Streambuf<char>;
using SbW = Streambuf<char16_t>;

#define SB_EXPORTS(T, E, P)                                                                                        \
    HLE_EXPORT_AS(msvcp110, "??0?$basic_streambuf@" T "@std@@IEAA@XZ", sb_ctor<E>);                            \
    HLE_EXPORT_AS(msvcp110, "??1?$basic_streambuf@" T "@std@@UEAA@XZ", sb_dtor<E>);                            \
    HLE_EXPORT_AS(msvcp110, "?_Lock@?$basic_streambuf@" T "@std@@UEAAXXZ", sb_nop<E>);                         \
    HLE_EXPORT_AS(msvcp110, "?_Unlock@?$basic_streambuf@" T "@std@@UEAAXXZ", sb_nop<E>);                       \
    HLE_EXPORT_AS(msvcp110, "?showmanyc@?$basic_streambuf@" T "@std@@MEAA_JXZ", sb_showmanyc<E>);              \
    HLE_EXPORT_AS(msvcp110, "?sync@?$basic_streambuf@" T "@std@@MEAAHXZ", sb_sync<E>);                         \
    HLE_EXPORT_AS(msvcp110, "?imbue@?$basic_streambuf@" T "@std@@MEAAXAEBVlocale@2@@Z", sb_imbue<E>);          \
    HLE_EXPORT_AS(msvcp110, "?xsgetn@?$basic_streambuf@" T "@std@@MEAA_JPEA" P "_J@Z", sb_xsgetn<E>);          \
    HLE_EXPORT_AS(msvcp110, "?xsputn@?$basic_streambuf@" T "@std@@MEAA_JPEB" P "_J@Z", sb_xsputn<E>);          \
    HLE_EXPORT_AS(msvcp110, "?setbuf@?$basic_streambuf@" T "@std@@MEAAPEAV12@PEA" P "_J@Z", sb_setbuf<E>);     \
    HLE_EXPORT_AS(msvcp110, "?pptr@?$basic_streambuf@" T "@std@@IEBAPEA" P "XZ", sb_pptr<E>);                  \
    HLE_EXPORT_AS(msvcp110, "?_Pninc@?$basic_streambuf@" T "@std@@IEAAPEA" P "XZ", sb_pninc<E>);               \
    HLE_EXPORT_AS(msvcp110, "?setg@?$basic_streambuf@" T "@std@@IEAAXPEA" P "00@Z", sb_setg<E>);               \
    HLE_EXPORT_AS(msvcp110, "?setp@?$basic_streambuf@" T "@std@@IEAAXPEA" P "0@Z", sb_setp<E>);                \
    HLE_EXPORT_AS(msvcp110, "?sputn@?$basic_streambuf@" T "@std@@QEAA_JPEB" P "_J@Z", sb_sputn<E>)

#define TD "DU?$char_traits@D@std@@"
#define TW "_WU?$char_traits@_W@std@@"
SB_EXPORTS(TD, char, "D");
SB_EXPORTS(TW, char16_t, "_W");

HLE_EXPORT_AS(msvcp110, "?uflow@?$basic_streambuf@" TD "@std@@MEAAHXZ", sb_uflow<char>);
HLE_EXPORT_AS(msvcp110, "?uflow@?$basic_streambuf@" TW "@std@@MEAAGXZ", sb_uflow<char16_t>);
HLE_EXPORT_AS(msvcp110, "?sbumpc@?$basic_streambuf@" TD "@std@@QEAAHXZ", sb_sbumpc<char>);
HLE_EXPORT_AS(msvcp110, "?sbumpc@?$basic_streambuf@" TW "@std@@QEAAGXZ", sb_sbumpc<char16_t>);
HLE_EXPORT_AS(msvcp110, "?sgetc@?$basic_streambuf@" TD "@std@@QEAAHXZ", sb_sgetc<char>);
HLE_EXPORT_AS(msvcp110, "?sgetc@?$basic_streambuf@" TW "@std@@QEAAGXZ", sb_sgetc<char16_t>);
HLE_EXPORT_AS(msvcp110, "?snextc@?$basic_streambuf@" TD "@std@@QEAAHXZ", sb_snextc<char>);
HLE_EXPORT_AS(msvcp110, "?snextc@?$basic_streambuf@" TW "@std@@QEAAGXZ", sb_snextc<char16_t>);
HLE_EXPORT_AS(msvcp110, "?sputc@?$basic_streambuf@" TD "@std@@QEAAHD@Z", sb_sputc<char>);
HLE_EXPORT_AS(msvcp110, "?sputc@?$basic_streambuf@" TW "@std@@QEAAG_W@Z", sb_sputc<char16_t>);
HLE_EXPORT_AS(msvcp110, "?eback@?$basic_streambuf@" TD "@std@@IEBAPEADXZ", sb_eback<char>);
HLE_EXPORT_AS(msvcp110, "?egptr@?$basic_streambuf@" TD "@std@@IEBAPEADXZ", sb_egptr<char>);
HLE_EXPORT_AS(msvcp110, "?epptr@?$basic_streambuf@" TD "@std@@IEBAPEADXZ", sb_epptr<char>);

// ios_base / basic_ios
HLE_EXPORT_AS(msvcp110, "??_7ios_base@std@@6B@", g_ios_base_vt);
HLE_EXPORT_AS(msvcp110, "?_Ios_base_dtor@ios_base@std@@CAXPEAV12@@Z", ios_base_dtor);
HLE_EXPORT_AS(msvcp110, "?flags@ios_base@std@@QEBAHXZ", ios_flags);
HLE_EXPORT_AS(msvcp110, "?width@ios_base@std@@QEBA_JXZ", ios_width_get);
HLE_EXPORT_AS(msvcp110, "?width@ios_base@std@@QEAA_J_J@Z", ios_width_set);
HLE_EXPORT_AS(msvcp110, "?getloc@ios_base@std@@QEBA?AVlocale@2@XZ", ios_getloc);
HLE_EXPORT_AS(msvcp110, "??0?$basic_ios@" TD "@std@@IEAA@XZ", bios_ctor<char>);
HLE_EXPORT_AS(msvcp110, "??0?$basic_ios@" TW "@std@@IEAA@XZ", bios_ctor<char16_t>);
HLE_EXPORT_AS(msvcp110, "??1?$basic_ios@" TD "@std@@UEAA@XZ", bios_dtor<char>);
HLE_EXPORT_AS(msvcp110, "??1?$basic_ios@" TW "@std@@UEAA@XZ", bios_dtor<char16_t>);
HLE_EXPORT_AS(msvcp110, "??_7?$basic_ios@" TD "@std@@6B@", bios_vt<char>);
HLE_EXPORT_AS(msvcp110, "??_7?$basic_ios@" TW "@std@@6B@", bios_vt<char16_t>);
HLE_EXPORT_AS(msvcp110, "?_Add_vtordisp1@?$basic_ios@" TD "@std@@UEAAXXZ", vt_nop);
HLE_EXPORT_AS(msvcp110, "?_Add_vtordisp1@?$basic_ios@" TW "@std@@UEAAXXZ", vt_nop);
HLE_EXPORT_AS(msvcp110, "?_Add_vtordisp2@?$basic_ios@" TD "@std@@UEAAXXZ", vt_nop);
HLE_EXPORT_AS(msvcp110, "?_Add_vtordisp2@?$basic_ios@" TW "@std@@UEAAXXZ", vt_nop);
HLE_EXPORT_AS(msvcp110, "?clear@?$basic_ios@" TD "@std@@QEAAXH_N@Z", bios_clear_export<char>);
HLE_EXPORT_AS(msvcp110, "?setstate@?$basic_ios@" TD "@std@@QEAAXH_N@Z", bios_setstate_export<char>);
HLE_EXPORT_AS(msvcp110, "?setstate@?$basic_ios@" TW "@std@@QEAAXH_N@Z", bios_setstate_export<char16_t>);
HLE_EXPORT_AS(msvcp110, "?rdbuf@?$basic_ios@" TW "@std@@QEBAPEAV?$basic_streambuf@" TW "@2@XZ", bios_rdbuf<char16_t>);
HLE_EXPORT_AS(msvcp110, "?widen@?$basic_ios@" TW "@std@@QEBA_WD@Z", bios_widen_w);

// basic_ostream
HLE_EXPORT_AS(msvcp110, "??0?$basic_ostream@" TD "@std@@QEAA@PEAV?$basic_streambuf@" TD "@1@_N@Z", ostream_ctor<char>);
HLE_EXPORT_AS(msvcp110, "??0?$basic_ostream@" TW "@std@@QEAA@PEAV?$basic_streambuf@" TW "@1@_N@Z",
              ostream_ctor<char16_t>);
HLE_EXPORT_AS(msvcp110, "??1?$basic_ostream@" TD "@std@@UEAA@XZ", ostream_dtor<char>);
HLE_EXPORT_AS(msvcp110, "??1?$basic_ostream@" TW "@std@@UEAA@XZ", ostream_dtor<char16_t>);
HLE_EXPORT_AS(msvcp110, "??_7?$basic_ostream@" TD "@std@@6B@", ostream_vt<char>);
HLE_EXPORT_AS(msvcp110, "?_Add_vtordisp2@?$basic_ostream@" TD "@std@@UEAAXXZ", vt_nop);
HLE_EXPORT_AS(msvcp110, "?_Add_vtordisp2@?$basic_ostream@" TW "@std@@UEAAXXZ", vt_nop);
HLE_EXPORT_AS(msvcp110, "?_Osfx@?$basic_ostream@" TD "@std@@QEAAXXZ", ostream_osfx<char>);
HLE_EXPORT_AS(msvcp110, "?_Osfx@?$basic_ostream@" TW "@std@@QEAAXXZ", ostream_osfx<char16_t>);
HLE_EXPORT_AS(msvcp110, "?flush@?$basic_ostream@" TD "@std@@QEAAAEAV12@XZ", ostream_flush<char>);
HLE_EXPORT_AS(msvcp110, "?flush@?$basic_ostream@" TW "@std@@QEAAAEAV12@XZ", ostream_flush<char16_t>);
#define OPUT(T, E, M, fn) HLE_EXPORT_AS(msvcp110, "??6?$basic_ostream@" T "@std@@QEAAAEAV01@" M "@Z", fn)
OPUT(TD, char, "F", (ostream_put_int<char, int16_t>));
OPUT(TD, char, "G", (ostream_put_int<char, uint16_t>));
OPUT(TD, char, "H", (ostream_put_int<char, int32_t>));
OPUT(TD, char, "I", (ostream_put_int<char, uint32_t>));
OPUT(TD, char, "J", (ostream_put_int<char, int32_t>));   // long is 32-bit
OPUT(TD, char, "K", (ostream_put_int<char, uint32_t>));  // unsigned long
OPUT(TD, char, "_J", (ostream_put_int<char, int64_t>));
OPUT(TD, char, "_K", (ostream_put_int<char, uint64_t>));
OPUT(TD, char, "M", ostream_put_float<char>);
OPUT(TD, char, "N", ostream_put_double<char>);
OPUT(TD, char, "_N", ostream_put_bool<char>);
OPUT(TD, char, "P6AAEAVios_base@1@AEAV21@@Z", ostream_put_manip);
OPUT(TW, char16_t, "H", (ostream_put_int<char16_t, int32_t>));
OPUT(TW, char16_t, "N", ostream_put_double<char16_t>);
OPUT(TW, char16_t, "_K", (ostream_put_int<char16_t, uint64_t>));

// basic_istream
HLE_EXPORT_AS(msvcp110, "??0?$basic_istream@" TD "@std@@QEAA@PEAV?$basic_streambuf@" TD "@1@_N@Z", istream_ctor<char>);
HLE_EXPORT_AS(msvcp110, "??0?$basic_istream@" TW "@std@@QEAA@PEAV?$basic_streambuf@" TW "@1@_N@Z",
              istream_ctor<char16_t>);
HLE_EXPORT_AS(msvcp110, "??1?$basic_istream@" TD "@std@@UEAA@XZ", istream_dtor<char>);
HLE_EXPORT_AS(msvcp110, "??1?$basic_istream@" TW "@std@@UEAA@XZ", istream_dtor<char16_t>);
HLE_EXPORT_AS(msvcp110, "?_Add_vtordisp1@?$basic_istream@" TD "@std@@UEAAXXZ", vt_nop);
HLE_EXPORT_AS(msvcp110, "?_Add_vtordisp1@?$basic_istream@" TW "@std@@UEAAXXZ", vt_nop);
HLE_EXPORT_AS(msvcp110, "?_Ipfx@?$basic_istream@" TD "@std@@QEAA_N_N@Z", istream_ipfx<char>);
HLE_EXPORT_AS(msvcp110, "?_Ipfx@?$basic_istream@" TW "@std@@QEAA_N_N@Z", istream_ipfx<char16_t>);
#define IGET(T, M, fn) HLE_EXPORT_AS(msvcp110, "??5?$basic_istream@" T "@std@@QEAAAEAV01@AEA" M "@Z", fn)
IGET(TD, "F", (istream_get_int<char, int16_t>));
IGET(TD, "G", (istream_get_int<char, uint16_t>));
IGET(TD, "H", (istream_get_int<char, int32_t>));
IGET(TD, "I", (istream_get_int<char, uint32_t>));
IGET(TD, "J", (istream_get_int<char, int32_t>));
IGET(TD, "K", (istream_get_int<char, uint32_t>));
IGET(TD, "_J", (istream_get_int<char, int64_t>));
IGET(TD, "_K", (istream_get_int<char, uint64_t>));
IGET(TD, "M", (istream_get_float<char, float>));
IGET(TD, "N", (istream_get_float<char, double>));
IGET(TW, "H", (istream_get_int<char16_t, int32_t>));
IGET(TW, "N", (istream_get_float<char16_t, double>));

// basic_iostream
HLE_EXPORT_AS(msvcp110, "??0?$basic_iostream@" TD "@std@@QEAA@PEAV?$basic_streambuf@" TD "@1@@Z", iostream_ctor<char>);
HLE_EXPORT_AS(msvcp110, "??0?$basic_iostream@" TW "@std@@QEAA@PEAV?$basic_streambuf@" TW "@1@@Z",
              iostream_ctor<char16_t>);
HLE_EXPORT_AS(msvcp110, "??1?$basic_iostream@" TD "@std@@UEAA@XZ", iostream_dtor<char>);
HLE_EXPORT_AS(msvcp110, "??1?$basic_iostream@" TW "@std@@UEAA@XZ", iostream_dtor<char16_t>);

// misc
HLE_EXPORT_AS(msvcp110, "?cout@std@@3V?$basic_ostream@DU?$char_traits@D@std@@@1@A", g_cout);
HLE_EXPORT_AS(msvcp110, "?setw@std@@YA?AU?$_Smanip@_J@1@_J@Z", setw_);
HLE_EXPORT_AS(msvcp110, "?setprecision@std@@YA?AU?$_Smanip@_J@1@_J@Z", setprecision_);
HLE_EXPORT_AS(msvcp110, "?_1@placeholders@std@@3V?$_Ph@$00@2@A", g_placeholder1);
HLE_EXPORT_AS(msvcp110, "?_2@placeholders@std@@3V?$_Ph@$01@2@A", g_placeholder2);

}  // namespace fl::crt::msvcp
