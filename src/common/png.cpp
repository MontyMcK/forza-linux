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

void chunk(FILE* f, const char* type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out;
    put32(out, static_cast<uint32_t>(data.size()));
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put32(out, static_cast<uint32_t>(crc32(0, out.data() + 4, static_cast<uInt>(data.size() + 4))));
    fwrite(out.data(), 1, out.size(), f);
}

}  // namespace

bool write_png(const std::string& path, const uint8_t* rgba, int width, int height) {
    std::vector<uint8_t> raw;
    raw.reserve(size_t(height) * (size_t(width) * 4 + 1));
    for (int y = 0; y < height; y++) {
        raw.push_back(0);  // filter: none
        raw.insert(raw.end(), rgba + size_t(y) * width * 4, rgba + size_t(y + 1) * width * 4);
    }
    uLongf zlen = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> z(zlen);
    if (compress2(z.data(), &zlen, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK) return false;
    z.resize(zlen);

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr;
    put32(ihdr, width);
    put32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8-bit RGBA, no interlace
    chunk(f, "IHDR", ihdr);
    chunk(f, "IDAT", z);
    chunk(f, "IEND", {});
    return fclose(f) == 0;
}

}  // namespace fl
