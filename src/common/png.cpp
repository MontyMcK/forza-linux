#include "common/png.h"

#include <cstdio>
#include <vector>
#include <zlib.h>

namespace fl {
namespace {

void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x >> 24);
    v.push_back(x >> 16);
    v.push_back(x >> 8);
    v.push_back(x);
}

void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    size_t start = out.size();
    put32(out, static_cast<uint32_t>(data.size()));
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put32(out, static_cast<uint32_t>(crc32(0, out.data() + start + 4, static_cast<uInt>(data.size() + 4))));
}

}  // namespace

std::vector<uint8_t> encode_png(const uint8_t* rgba, int width, int height, size_t stride) {
    std::vector<uint8_t> raw;
    raw.reserve(size_t(height) * (size_t(width) * 4 + 1));
    for (int y = 0; y < height; y++) {
        raw.push_back(0);  // filter: none
        raw.insert(raw.end(), rgba + size_t(y) * stride, rgba + size_t(y) * stride + size_t(width) * 4);
    }
    uLongf zlen = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> z(zlen);
    if (compress2(z.data(), &zlen, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK) return {};
    z.resize(zlen);

    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<uint8_t> out(sig, sig + 8);
    std::vector<uint8_t> ihdr;
    put32(ihdr, width);
    put32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8-bit RGBA, no interlace
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    return out;
}

bool write_png(const std::string& path, const uint8_t* rgba, int width, int height) {
    std::vector<uint8_t> png = encode_png(rgba, width, height, size_t(width) * 4);
    if (png.empty()) return false;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fwrite(png.data(), 1, png.size(), f);
    return fclose(f) == 0;
}

}  // namespace fl
