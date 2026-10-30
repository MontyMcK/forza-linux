// Runs the game's WIC PNG encode sequence (rva 0x1130130, FINDINGS "WIC") against
// src/hle/wic/wic.cpp and decodes the PNG it reads back from the stream.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <zlib.h>

#include "common/winabi.h"
#include "hle/registry.h"
#include "hle/winrt/com.h"

// What wic.cpp needs from the rest of the runtime.
namespace fl::loader {
[[noreturn]] WINAPI void test_trap() {
    std::printf("FAIL  an unimplemented WIC slot was called\n");
    std::exit(1);
}
void* make_named_trap(const std::string&, const std::string&) { return reinterpret_cast<void*>(&test_trap); }
}  // namespace fl::loader
namespace fl::winrt {
std::string guid_str(const GUID& g) {
    char b[40];
    std::snprintf(b, sizeof b, "{%08X-...}", g.Data1);
    return b;
}
HRESULT make_hstring(const char16_t*, HSTRING* out) {
    *out = nullptr;
    return E_NOTIMPL;
}
}  // namespace fl::winrt

namespace {

constexpr GUID kPng = {0x1B7CFAF4, 0x713F, 0x473C, {0xBB, 0xCD, 0x61, 0x37, 0x42, 0x5F, 0xAE, 0xAF}};
constexpr GUID kBGRA = {0x6FDDC324, 0x4E03, 0x4BFE, {0xB1, 0x85, 0x3D, 0x77, 0x76, 0x8D, 0xC9, 0x0F}};
constexpr GUID kRGBA = {0xF5C7AD2D, 0x6A8D, 0x43DD, {0xA7, 0xA8, 0xA2, 0x99, 0x35, 0x26, 0x1A, 0xE9}};

int g_failed = 0;
void expect(bool ok, const char* what) {
    g_failed += !ok;
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
}

template <typename R, typename... A>
R call(void* obj, int slot, A... args) {
    auto** vt = *static_cast<void***>(obj);
    return reinterpret_cast<R(WINAPI*)(void*, A...)>(vt[slot])(obj, args...);
}
void release(void* o) { call<ULONG>(o, 2); }

struct Rect {
    int32_t x, y, w, h;
};

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

// RGBA8 rows from our (unfiltered, single IDAT) PNG, empty if malformed.
std::vector<uint8_t> decode(const std::vector<uint8_t>& png, uint32_t* w, uint32_t* h) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (png.size() < 8 || std::memcmp(png.data(), sig, 8)) return {};
    std::vector<uint8_t> z;
    for (size_t p = 8; p + 12 <= png.size();) {
        uint32_t n = be32(&png[p]);
        const uint8_t* type = &png[p + 4];
        if (!std::memcmp(type, "IHDR", 4)) *w = be32(&png[p + 8]), *h = be32(&png[p + 12]);
        if (!std::memcmp(type, "IDAT", 4)) z.insert(z.end(), &png[p + 8], &png[p + 8 + n]);
        if (crc32(0, &png[p + 4], n + 4) != be32(&png[p + 8 + n])) return {};
        p += 12 + n;
    }
    std::vector<uint8_t> raw(size_t(*h) * (*w * 4 + 1));
    uLongf len = raw.size();
    if (uncompress(raw.data(), &len, z.data(), z.size()) != Z_OK || len != raw.size()) return {};
    std::vector<uint8_t> out;
    for (uint32_t y = 0; y < *h; y++) out.insert(out.end(), &raw[y * (*w * 4 + 1) + 1], &raw[(y + 1) * (*w * 4 + 1)]);
    return out;
}

// The encode as the game does it, with the source pixels in `src_fmt`.
std::vector<uint8_t> game_encode(void* factory, const uint8_t* px, UINT w, UINT h, UINT stride, const GUID& src_fmt) {
    using CreateStream = HRESULT(WINAPI*)(void*, BOOL, void**);
    auto create_stream = reinterpret_cast<CreateStream>(fl::hle::lookup("combase", "CreateStreamOnHGlobal"));
    void *stream = nullptr, *encoder = nullptr, *frame = nullptr, *props = nullptr;
    if (create_stream(nullptr, 1, &stream) < 0) return {};
    if (call<HRESULT>(factory, 8, &kPng, static_cast<const GUID*>(nullptr), &encoder) < 0) return {};
    if (call<HRESULT>(encoder, 3, stream, 2) < 0) return {};
    if (call<HRESULT>(encoder, 10, &frame, &props) < 0) return {};
    if (call<HRESULT>(frame, 3, props) < 0) return {};
    if (call<HRESULT>(frame, 4, w, h) < 0) return {};
    if (call<HRESULT>(frame, 5, 72.0, 72.0) < 0) return {};
    GUID fmt = kBGRA;
    if (call<HRESULT>(frame, 6, &fmt) < 0) return {};
    if (src_fmt == fmt) {
        if (call<HRESULT>(frame, 10, h, stride, stride * h, px) < 0) return {};
    } else {
        void *bitmap = nullptr, *conv = nullptr;
        if (call<HRESULT>(factory, 20, w, h, &src_fmt, stride, stride * h, px, &bitmap) < 0) return {};
        if (call<HRESULT>(factory, 10, &conv) < 0) return {};
        if (call<HRESULT>(conv, 8, bitmap, &kBGRA, 0, static_cast<void*>(nullptr), 0.0, 0) < 0) return {};
        Rect r{0, 0, int32_t(w), int32_t(h)};
        if (call<HRESULT>(frame, 11, conv, &r) < 0) return {};
        release(conv);
        release(bitmap);
    }
    if (call<HRESULT>(frame, 12) < 0 || call<HRESULT>(encoder, 11) < 0) return {};
    release(props);
    release(frame);
    release(encoder);
    uint8_t stat[80] = {};
    if (call<HRESULT>(stream, 12, stat, 1) < 0) return {};
    uint64_t size;
    std::memcpy(&size, stat + 16, 8);
    std::vector<uint8_t> out(size);
    ULONG got = 0;
    if (call<HRESULT>(stream, 5, int64_t(0), DWORD(0), static_cast<uint64_t*>(nullptr)) < 0) return {};
    if (call<HRESULT>(stream, 3, out.data(), ULONG(size), &got) < 0 || got != size) return {};
    release(stream);
    return out;
}

}  // namespace

int main() {
    using CreateFactory = HRESULT(WINAPI*)(UINT, void**);
    auto create = reinterpret_cast<CreateFactory>(fl::hle::lookup("windowscodecs", "WICCreateImagingFactory_Proxy"));
    void* factory = nullptr;
    if (!create || create(0x237, &factory) < 0) {
        std::printf("FAIL  WICCreateImagingFactory_Proxy\n");
        return 1;
    }

    // R8G8B8A8 thumbnail pixels (what the game passes): the converter path.
    const uint8_t rgba[3 * 2 * 4] = {255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0,
                                     10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120};
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> png = game_encode(factory, rgba, 3, 2, 12, kRGBA);
    std::vector<uint8_t> back = decode(png, &w, &h);
    expect(w == 3 && h == 2 && back.size() == sizeof rgba && !std::memcmp(back.data(), rgba, sizeof rgba),
           "32bppRGBA source via the format converter: PNG decodes to the same pixels");

    // B8G8R8A8 with padded rows: written directly.
    uint8_t bgra[2 * 8] = {};
    const uint8_t want[2 * 4] = {1, 2, 3, 4, 5, 6, 7, 8};  // RGBA, one pixel per row
    for (int y = 0; y < 2; y++) {
        bgra[y * 8 + 0] = want[y * 4 + 2], bgra[y * 8 + 1] = want[y * 4 + 1];
        bgra[y * 8 + 2] = want[y * 4 + 0], bgra[y * 8 + 3] = want[y * 4 + 3];
    }
    png = game_encode(factory, bgra, 1, 2, 8, kBGRA);
    back = decode(png, &w, &h);
    expect(w == 1 && h == 2 && back.size() == sizeof want && !std::memcmp(back.data(), want, sizeof want),
           "32bppBGRA source with row padding: WritePixels, channels swapped to RGBA");
    release(factory);

    std::printf("%s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
