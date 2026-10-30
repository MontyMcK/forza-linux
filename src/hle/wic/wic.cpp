// Windows Imaging Component, just what the game's PNG encode needs, plus
// combase's CreateStreamOnHGlobal (the memory stream it encodes into).
//
// The game creates its factory at startup through LoadLibrary("WindowsCodecs.dll")
// + GetProcAddress("WICCreateImagingFactory_Proxy") (rva 0x112dce0) and encodes
// with it at rva 0x1130130 (car thumbnails when buying a car): CreateStreamOnHGlobal,
// factory CreateEncoder(PNG), encoder Initialize(stream, NoCache) / CreateNewFrame,
// frame Initialize / SetSize / SetResolution / SetPixelFormat, then WritePixels when
// its source format is 32bppBGRA or else CreateBitmapFromMemory + a format
// converter + WriteSource (the thumbnails are R8G8B8A8, so 32bppRGBA -> BGRA),
// frame and encoder Commit, and it reads the bytes back with IStream Stat / Seek /
// Read. Without WIC the stream's factory was null and buying a car crashed (on
// CreateStreamOnHGlobal, an unimplemented import), or hung on "Saving..." when the
// encode failed. Slot numbers are the public Windows SDK interfaces'.
#include <algorithm>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include "common/log.h"
#include "common/png.h"
#include "common/winabi.h"
#include "hle/registry.h"
#include "hle/winrt/object.h"

namespace fl::wic {
namespace {

using winrt::Iface;
using winrt::Object;
using winrt::Vtable;

constexpr HRESULT WINCODEC_ERR_WRONGSTATE = static_cast<HRESULT>(0x88982F04);
constexpr HRESULT WINCODEC_ERR_COMPONENTNOTFOUND = static_cast<HRESULT>(0x88982F50);
constexpr HRESULT WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT = static_cast<HRESULT>(0x88982F80);
constexpr HRESULT WINCODEC_ERR_UNSUPPORTEDOPERATION = static_cast<HRESULT>(0x88982F81);
constexpr HRESULT STG_E_INVALIDFUNCTION = static_cast<HRESULT>(0x80030001);

constexpr GUID IID_IStream = {0x0000000C, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr GUID IID_ISequentialStream = {0x0C733A30, 0x2A1C, 0x11CE, {0xAD, 0xE5, 0x00, 0xAA, 0x00, 0x44, 0x77, 0x3D}};
constexpr GUID IID_IWICImagingFactory = {0xEC5EC8A9, 0xC395, 0x4314, {0x9C, 0x77, 0x54, 0xD7, 0xA9, 0x35, 0xFF, 0x70}};
constexpr GUID IID_IWICBitmapEncoder = {0x00000103, 0xA8F2, 0x4877, {0xBA, 0x0A, 0xFD, 0x2B, 0x66, 0x45, 0xFB, 0x94}};
constexpr GUID IID_IWICBitmapFrameEncode = {0x00000105, 0xA8F2, 0x4877, {0xBA, 0x0A, 0xFD, 0x2B, 0x66, 0x45, 0xFB, 0x94}};
constexpr GUID IID_IWICBitmapSource = {0x00000120, 0xA8F2, 0x4877, {0xBA, 0x0A, 0xFD, 0x2B, 0x66, 0x45, 0xFB, 0x94}};
constexpr GUID IID_IWICBitmap = {0x00000121, 0xA8F2, 0x4877, {0xBA, 0x0A, 0xFD, 0x2B, 0x66, 0x45, 0xFB, 0x94}};
constexpr GUID IID_IWICFormatConverter = {0x00000301, 0xA8F2, 0x4877, {0xBA, 0x0A, 0xFD, 0x2B, 0x66, 0x45, 0xFB, 0x94}};
constexpr GUID IID_IPropertyBag2 = {0x22F55882, 0x280B, 0x11D0, {0xA8, 0xA9, 0x00, 0xA0, 0xC9, 0x0C, 0x20, 0x04}};
constexpr GUID GUID_ContainerFormatPng = {0x1B7CFAF4, 0x713F, 0x473C, {0xBB, 0xCD, 0x61, 0x37, 0x42, 0x5F, 0xAE, 0xAF}};
constexpr GUID GUID_WICPixelFormat32bppBGR = {0x6FDDC324, 0x4E03, 0x4BFE, {0xB1, 0x85, 0x3D, 0x77, 0x76, 0x8D, 0xC9, 0x0E}};
constexpr GUID GUID_WICPixelFormat32bppBGRA = {0x6FDDC324, 0x4E03, 0x4BFE, {0xB1, 0x85, 0x3D, 0x77, 0x76, 0x8D, 0xC9, 0x0F}};
constexpr GUID GUID_WICPixelFormat32bppRGBA = {0xF5C7AD2D, 0x6A8D, 0x43DD, {0xA7, 0xA8, 0xA2, 0x99, 0x35, 0x26, 0x1A, 0xE9}};

// A factory method we don't have: say which, and fail (callers check HRESULTs;
// the game coped with no factory at all before).
template <int Slot>
WINAPI HRESULT factory_unimplemented(Iface*) {
    static std::once_flag once;
    std::call_once(once, [] { FL_ERROR("UNIMPLEMENTED IWICImagingFactory slot %d", Slot); });
    return WINCODEC_ERR_UNSUPPORTEDOPERATION;
}
template <int... Slots>
void fail_slots(Vtable& vt, std::integer_sequence<int, Slots...>) {
    ((Slots >= 3 ? (void)vt.set(Slots, &factory_unimplemented<Slots>) : (void)0), ...);
}

// ---------------------------------------------------------------- IStream on memory

struct STATSTG {
    char16_t* pwcsName;
    DWORD type;
    uint64_t cbSize;
    uint64_t mtime, ctime, atime;
    DWORD grfMode, grfLocksSupported;
    GUID clsid;
    DWORD grfStateBits, reserved;
};
static_assert(sizeof(STATSTG) == 80 && offsetof(STATSTG, cbSize) == 16);

struct MemStream : Object {
    MemStream();
    Iface istream;
    std::mutex m;
    std::vector<uint8_t> data;
    size_t pos = 0;
};

WINAPI HRESULT stream_Read(Iface* self, void* out, ULONG n, ULONG* done) {
    auto* s = self->as<MemStream>();
    std::lock_guard lock(s->m);
    size_t k = s->pos < s->data.size() ? std::min<size_t>(n, s->data.size() - s->pos) : 0;
    if (k) std::memcpy(out, s->data.data() + s->pos, k);
    s->pos += k;
    if (done) *done = static_cast<ULONG>(k);
    return k == n ? S_OK : S_FALSE;
}

WINAPI HRESULT stream_Write(Iface* self, const void* in, ULONG n, ULONG* done) {
    auto* s = self->as<MemStream>();
    std::lock_guard lock(s->m);
    if (s->pos + n > s->data.size()) s->data.resize(s->pos + n);
    std::memcpy(s->data.data() + s->pos, in, n);
    s->pos += n;
    if (done) *done = n;
    return S_OK;
}

WINAPI HRESULT stream_Seek(Iface* self, int64_t move, DWORD origin, uint64_t* new_pos) {
    auto* s = self->as<MemStream>();
    std::lock_guard lock(s->m);
    int64_t base = origin == 0 ? 0 : origin == 1 ? int64_t(s->pos) : origin == 2 ? int64_t(s->data.size()) : -1;
    if (base < 0 || base + move < 0) return STG_E_INVALIDFUNCTION;
    s->pos = size_t(base + move);
    if (new_pos) *new_pos = s->pos;
    return S_OK;
}

WINAPI HRESULT stream_SetSize(Iface* self, uint64_t size) {
    auto* s = self->as<MemStream>();
    std::lock_guard lock(s->m);
    s->data.resize(size);
    return S_OK;
}

WINAPI HRESULT stream_Stat(Iface* self, STATSTG* st, DWORD flags) {
    auto* s = self->as<MemStream>();
    std::lock_guard lock(s->m);
    std::memset(st, 0, sizeof *st);  // pwcsName stays null (STATFLAG_NONAME or not)
    st->type = 2;                     // STGTY_STREAM
    st->cbSize = s->data.size();
    return S_OK;
}

Vtable& stream_vt() {
    static Vtable v = Vtable("IStream (memory)", 14, false)
                          .set(3, &stream_Read)
                          .set(4, &stream_Write)
                          .set(5, &stream_Seek)
                          .set(6, &stream_SetSize)
                          .set(12, &stream_Stat);
    return v;
}

MemStream::MemStream() : Object(u"MemoryStream"), istream(stream_vt(), this) {
    expose(IID_IStream, &istream);
    expose(IID_ISequentialStream, &istream);
}

// ---------------------------------------------------------------- pixels
// The formats the game's DXGI -> WIC table (rva 0x112e400) produces: R8G8B8A8
// -> 32bppRGBA, B8G8R8A8 -> 32bppBGRA, B8G8R8X8 -> 32bppBGR.

bool supported(const GUID& f) {
    return f == GUID_WICPixelFormat32bppRGBA || f == GUID_WICPixelFormat32bppBGRA || f == GUID_WICPixelFormat32bppBGR;
}

void convert_row(const uint8_t* src, const GUID& sf, uint8_t* dst, const GUID& df, UINT width) {
    bool sbgr = sf != GUID_WICPixelFormat32bppRGBA, dbgr = df != GUID_WICPixelFormat32bppRGBA;
    bool opaque = sf == GUID_WICPixelFormat32bppBGR;
    for (UINT x = 0; x < width; x++, src += 4, dst += 4) {
        uint8_t r = src[sbgr ? 2 : 0], g = src[1], b = src[sbgr ? 0 : 2], a = opaque ? 0xFF : src[3];
        dst[0] = dbgr ? b : r, dst[1] = g, dst[2] = dbgr ? r : b, dst[3] = a;
    }
}

struct WICRect {
    int32_t x, y, w, h;
};

// IWICBitmapSource slots: 3 GetSize, 4 GetPixelFormat, 5 GetResolution, 6 CopyPalette, 7 CopyPixels.
HRESULT source_size(void* src, UINT* w, UINT* h) { return winrt::call_slot<HRESULT>(src, 3, w, h); }
HRESULT source_format(void* src, GUID* f) { return winrt::call_slot<HRESULT>(src, 4, f); }
HRESULT source_copy(void* src, const WICRect* r, UINT stride, UINT size, uint8_t* out) {
    return winrt::call_slot<HRESULT>(src, 7, r, stride, size, out);
}

// The rect to copy (null = all), checked against the image; stride/size against it.
bool copy_rect(const WICRect* rc, UINT w, UINT h, UINT stride, UINT size, WICRect* out) {
    *out = rc ? *rc : WICRect{0, 0, int32_t(w), int32_t(h)};
    if (out->x < 0 || out->y < 0 || out->w <= 0 || out->h <= 0 || UINT(out->x + out->w) > w || UINT(out->y + out->h) > h)
        return false;
    return stride >= UINT(out->w) * 4 && size >= size_t(out->h - 1) * stride + size_t(out->w) * 4;
}

// IWICBitmap (CreateBitmapFromMemory): a copy of the caller's pixels.
struct Bitmap : Object {
    Bitmap();
    Iface ibitmap;
    UINT width = 0, height = 0;
    GUID format{};
    std::vector<uint8_t> px;  // rows of width * 4 bytes
};

WINAPI HRESULT bitmap_GetSize(Iface* self, UINT* w, UINT* h) {
    auto* b = self->as<Bitmap>();
    *w = b->width, *h = b->height;
    return S_OK;
}
WINAPI HRESULT bitmap_GetPixelFormat(Iface* self, GUID* f) {
    *f = self->as<Bitmap>()->format;
    return S_OK;
}
WINAPI HRESULT bitmap_CopyPixels(Iface* self, const WICRect* rc, UINT stride, UINT size, uint8_t* out) {
    auto* b = self->as<Bitmap>();
    WICRect r;
    if (!copy_rect(rc, b->width, b->height, stride, size, &r)) return E_INVALIDARG;
    for (int y = 0; y < r.h; y++)
        std::memcpy(out + size_t(y) * stride, b->px.data() + (size_t(r.y + y) * b->width + r.x) * 4, size_t(r.w) * 4);
    return S_OK;
}

Vtable& bitmap_vt() {
    static Vtable v = Vtable("IWICBitmap (memory)", 11, false)
                          .set(3, &bitmap_GetSize)
                          .set(4, &bitmap_GetPixelFormat)
                          .set(7, &bitmap_CopyPixels);
    return v;
}
Bitmap::Bitmap() : Object(u"WICBitmap"), ibitmap(bitmap_vt(), this) {
    expose(IID_IWICBitmap, &ibitmap);
    expose(IID_IWICBitmapSource, &ibitmap);
}

// IWICFormatConverter between the three formats above.
struct Converter : Object {
    Converter();
    ~Converter() override;
    Iface iconv;
    void* source = nullptr;  // a reference
    GUID from{}, to{};
    UINT width = 0, height = 0;
};

WINAPI HRESULT conv_GetSize(Iface* self, UINT* w, UINT* h) {
    auto* c = self->as<Converter>();
    if (!c->source) return WINCODEC_ERR_WRONGSTATE;
    *w = c->width, *h = c->height;
    return S_OK;
}
WINAPI HRESULT conv_GetPixelFormat(Iface* self, GUID* f) {
    auto* c = self->as<Converter>();
    if (!c->source) return WINCODEC_ERR_WRONGSTATE;
    *f = c->to;
    return S_OK;
}
WINAPI HRESULT conv_CopyPixels(Iface* self, const WICRect* rc, UINT stride, UINT size, uint8_t* out) {
    auto* c = self->as<Converter>();
    if (!c->source) return WINCODEC_ERR_WRONGSTATE;
    WICRect r;
    if (!copy_rect(rc, c->width, c->height, stride, size, &r)) return E_INVALIDARG;
    std::vector<uint8_t> tmp(size_t(r.w) * r.h * 4);
    HRESULT hr = source_copy(c->source, &r, UINT(r.w) * 4, UINT(tmp.size()), tmp.data());
    if (hr < 0) return hr;
    for (int y = 0; y < r.h; y++) convert_row(tmp.data() + size_t(y) * r.w * 4, c->from, out + size_t(y) * stride, c->to, r.w);
    return S_OK;
}
// Initialize(source, dst format, dither, palette, alpha threshold, palette type).
WINAPI HRESULT conv_Initialize(Iface* self, void* src, const GUID& to, int, void*, double, int) {
    auto* c = self->as<Converter>();
    if (c->source || !src) return WINCODEC_ERR_WRONGSTATE;
    GUID from{};
    HRESULT hr = source_format(src, &from);
    if (hr >= 0) hr = source_size(src, &c->width, &c->height);
    if (hr < 0) return hr;
    if (!supported(from) || !supported(to)) {
        FL_ERROR("WIC format converter: %s -> %s not supported", winrt::guid_str(from).c_str(), winrt::guid_str(to).c_str());
        return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    }
    winrt::com_add_ref(src);
    c->source = src, c->from = from, c->to = to;
    return S_OK;
}
WINAPI HRESULT conv_CanConvert(Iface*, const GUID& from, const GUID& to, BOOL* ok) {
    *ok = supported(from) && supported(to);
    return S_OK;
}

Vtable& conv_vt() {
    static Vtable v = Vtable("IWICFormatConverter", 10, false)
                          .set(3, &conv_GetSize)
                          .set(4, &conv_GetPixelFormat)
                          .set(7, &conv_CopyPixels)
                          .set(8, &conv_Initialize)
                          .set(9, &conv_CanConvert);
    return v;
}
Converter::Converter() : Object(u"WICFormatConverter"), iconv(conv_vt(), this) {
    expose(IID_IWICFormatConverter, &iconv);
    expose(IID_IWICBitmapSource, &iconv);
}
Converter::~Converter() {
    if (source) winrt::com_release(source);
}

// ---------------------------------------------------------------- PNG encoder

struct PropertyBag : Object {
    PropertyBag();
    Iface ibag;
};

// IPropertyBag2::Write(count, PROPBAG2*, VARIANT*): PNG has no options the game sets.
WINAPI HRESULT bag_Write(Iface*, ULONG, void*, void*) { return S_OK; }

Vtable& bag_vt() {
    static Vtable v = Vtable("IPropertyBag2 (WIC encoder options)", 8, false).set(4, &bag_Write);
    return v;
}
PropertyBag::PropertyBag() : Object(u"WICEncoderOptions"), ibag(bag_vt(), this) { expose(IID_IPropertyBag2, &ibag); }

struct Encoder;
struct Frame : Object {
    explicit Frame(Encoder* e);
    ~Frame() override;
    Iface iframe;
    Encoder* encoder;
    UINT width = 0, height = 0;
    GUID format = GUID_WICPixelFormat32bppBGRA;
    std::vector<uint8_t> rgba;  // rows written so far
    UINT rows = 0;
};

struct Encoder : Object {
    Encoder();
    ~Encoder() override;
    Iface iencoder;
    void* stream = nullptr;  // the caller's IStream (a reference)
    std::vector<uint8_t> png;
    bool committed = false;
};

WINAPI HRESULT frame_Initialize(Iface*, void*) { return S_OK; }

WINAPI HRESULT frame_SetSize(Iface* self, UINT w, UINT h) {
    auto* f = self->as<Frame>();
    if (!w || !h || w > 16384 || h > 16384) return E_INVALIDARG;
    f->width = w, f->height = h;
    f->rgba.assign(size_t(w) * h * 4, 0);
    return S_OK;
}

WINAPI HRESULT frame_SetResolution(Iface*, double, double) { return S_OK; }

// In/out: the format the encoder will take. PNG takes the three above as they are.
WINAPI HRESULT frame_SetPixelFormat(Iface* self, GUID* fmt) {
    auto* f = self->as<Frame>();
    if (supported(*fmt)) {
        f->format = *fmt;
        return S_OK;
    }
    FL_ERROR("WIC PNG encoder: pixel format %s not supported", winrt::guid_str(*fmt).c_str());
    return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
}

HRESULT frame_rows(Frame* f, UINT lines, UINT stride, UINT size, const uint8_t* px, const GUID& format) {
    if (!lines) return S_OK;
    if (!f->width || f->rows + lines > f->height || stride < f->width * 4 || size < size_t(lines - 1) * stride + f->width * 4)
        return E_INVALIDARG;
    for (UINT y = 0; y < lines; y++)
        convert_row(px + size_t(y) * stride, format, f->rgba.data() + size_t(f->rows + y) * f->width * 4,
                    GUID_WICPixelFormat32bppRGBA, f->width);
    f->rows += lines;
    return S_OK;
}

WINAPI HRESULT frame_WritePixels(Iface* self, UINT lines, UINT stride, UINT size, const uint8_t* px) {
    auto* f = self->as<Frame>();
    return frame_rows(f, lines, stride, size, px, f->format);
}

// WriteSource(source, rect): the game passes its format converter, full size.
WINAPI HRESULT frame_WriteSource(Iface* self, void* src, const WICRect* rc) {
    auto* f = self->as<Frame>();
    GUID fmt{};
    UINT w = 0, h = 0;
    HRESULT hr = source_format(src, &fmt);
    if (hr >= 0) hr = source_size(src, &w, &h);
    if (hr < 0) return hr;
    if (!supported(fmt)) {
        FL_ERROR("WIC PNG encoder: source format %s not supported", winrt::guid_str(fmt).c_str());
        return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    }
    WICRect r = rc ? *rc : WICRect{0, 0, int32_t(w), int32_t(h)};
    if (UINT(r.w) != f->width) return E_INVALIDARG;
    std::vector<uint8_t> px(size_t(r.w) * r.h * 4);
    hr = source_copy(src, &r, UINT(r.w) * 4, UINT(px.size()), px.data());
    return hr < 0 ? hr : frame_rows(f, UINT(r.h), UINT(r.w) * 4, UINT(px.size()), px.data(), fmt);
}

WINAPI HRESULT frame_Commit(Iface* self) {
    auto* f = self->as<Frame>();
    if (f->rows != f->height || !f->width) return WINCODEC_ERR_WRONGSTATE;
    f->encoder->png = encode_png(f->rgba.data(), int(f->width), int(f->height), size_t(f->width) * 4);
    return f->encoder->png.empty() ? E_OUTOFMEMORY : S_OK;
}

Vtable& frame_vt() {
    static Vtable v = Vtable("IWICBitmapFrameEncode (PNG)", 14, false)
                          .set(3, &frame_Initialize)
                          .set(4, &frame_SetSize)
                          .set(5, &frame_SetResolution)
                          .set(6, &frame_SetPixelFormat)
                          .set(10, &frame_WritePixels)
                          .set(11, &frame_WriteSource)
                          .set(12, &frame_Commit);
    return v;
}

Frame::Frame(Encoder* e) : Object(u"WICBitmapFrameEncode"), iframe(frame_vt(), this), encoder(e) {
    expose(IID_IWICBitmapFrameEncode, &iframe);
    encoder->add_ref();
}
Frame::~Frame() { encoder->release(); }

WINAPI HRESULT encoder_Initialize(Iface* self, void* stream, int cache_option) {
    auto* e = self->as<Encoder>();
    if (!stream) return E_INVALIDARG;
    if (e->stream) return WINCODEC_ERR_WRONGSTATE;
    winrt::com_add_ref(stream);
    e->stream = stream;
    return S_OK;
}

WINAPI HRESULT encoder_CreateNewFrame(Iface* self, void** frame, void** options) {
    auto* e = self->as<Encoder>();
    if (!e->stream) return WINCODEC_ERR_WRONGSTATE;
    *frame = (new Frame(e))->default_iface_noref();
    if (options) *options = (new PropertyBag())->default_iface_noref();
    return S_OK;
}

// Writes the PNG into the stream (through its own Write, slot 4).
WINAPI HRESULT encoder_Commit(Iface* self) {
    auto* e = self->as<Encoder>();
    if (e->committed || e->png.empty()) return WINCODEC_ERR_WRONGSTATE;
    ULONG done = 0;
    HRESULT hr = winrt::call_slot<HRESULT>(e->stream, 4, static_cast<const void*>(e->png.data()),
                                           static_cast<ULONG>(e->png.size()), &done);
    FL_LOG(wic, "PNG encoded: %zu bytes (hr 0x%08x)", e->png.size(), static_cast<uint32_t>(hr));
    e->committed = hr >= 0;
    return hr;
}

Vtable& encoder_vt() {
    static Vtable v = Vtable("IWICBitmapEncoder (PNG)", 13, false)
                          .set(3, &encoder_Initialize)
                          .set(10, &encoder_CreateNewFrame)
                          .set(11, &encoder_Commit);
    return v;
}

Encoder::Encoder() : Object(u"WICPngEncoder"), iencoder(encoder_vt(), this) { expose(IID_IWICBitmapEncoder, &iencoder); }
Encoder::~Encoder() {
    if (stream) winrt::com_release(stream);
}

// ---------------------------------------------------------------- factory

struct Factory : Object {
    Factory();
    Iface ifactory;
};

WINAPI HRESULT factory_CreateEncoder(Iface*, const GUID& container, const GUID* vendor, void** out) {
    *out = nullptr;
    if (container != GUID_ContainerFormatPng) {
        FL_ERROR("UNIMPLEMENTED WIC encoder for container %s", winrt::guid_str(container).c_str());
        return WINCODEC_ERR_COMPONENTNOTFOUND;
    }
    *out = (new Encoder())->default_iface_noref();
    return S_OK;
}

WINAPI HRESULT factory_CreateFormatConverter(Iface*, void** out) {
    *out = (new Converter())->default_iface_noref();
    return S_OK;
}

// CreateBitmapFromMemory(w, h, format, stride, size, pixels, IWICBitmap**): copies the pixels.
WINAPI HRESULT factory_CreateBitmapFromMemory(Iface*, UINT w, UINT h, const GUID& fmt, UINT stride, UINT size,
                                              const uint8_t* px, void** out) {
    *out = nullptr;
    if (!supported(fmt)) {
        FL_ERROR("UNIMPLEMENTED WIC bitmap in pixel format %s", winrt::guid_str(fmt).c_str());
        return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    }
    if (!w || !h || stride < w * 4 || size < size_t(h - 1) * stride + size_t(w) * 4 || !px) return E_INVALIDARG;
    auto* b = new Bitmap();
    b->width = w, b->height = h, b->format = fmt;
    b->px.resize(size_t(w) * h * 4);
    for (UINT y = 0; y < h; y++) std::memcpy(b->px.data() + size_t(y) * w * 4, px + size_t(y) * stride, size_t(w) * 4);
    *out = b->default_iface_noref();
    return S_OK;
}

Vtable& factory_vt() {
    static Vtable v = [] {
        Vtable t("IWICImagingFactory", 28, false);
        fail_slots(t, std::make_integer_sequence<int, 28>());
        t.set(8, &factory_CreateEncoder);
        t.set(10, &factory_CreateFormatConverter);
        t.set(20, &factory_CreateBitmapFromMemory);
        return t;
    }();
    return v;
}

Factory::Factory() : Object(u"WICImagingFactory"), ifactory(factory_vt(), this) {
    expose(IID_IWICImagingFactory, &ifactory);
}

WINAPI HRESULT WICCreateImagingFactory_Proxy_(UINT sdk_version, void** out) {
    if (!out) return E_INVALIDARG;
    FL_LOG(wic, "WICCreateImagingFactory_Proxy(0x%x)", sdk_version);
    *out = (new Factory())->default_iface_noref();
    return S_OK;
}

// CreateStreamOnHGlobal(hglobal, fDeleteOnRelease, IStream**): only new streams
// (hglobal null), the one way the game calls it.
WINAPI HRESULT CreateStreamOnHGlobal_(void* hglobal, BOOL delete_on_release, void** out) {
    if (!out) return E_INVALIDARG;
    *out = nullptr;
    if (hglobal) {
        FL_ERROR("UNIMPLEMENTED CreateStreamOnHGlobal on an existing HGLOBAL %p", hglobal);
        return E_NOTIMPL;
    }
    *out = (new MemStream())->default_iface_noref();
    return S_OK;
}

}  // namespace

HLE_EXPORT(windowscodecs, WICCreateImagingFactory_Proxy, WICCreateImagingFactory_Proxy_);
HLE_EXPORT(combase, CreateStreamOnHGlobal, CreateStreamOnHGlobal_);

}  // namespace fl::wic
