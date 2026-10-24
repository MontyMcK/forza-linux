// Dev tool: untile one mip of a .swatchbin texture with our GCN layout code and
// write it as a PNG, to check the untiler by eye. Reads game files, so write
// the output to research/private/ (Rule 0).
// Usage: swatch_view <file.swatchbin> <out.png> <bc4|r8> [tile=14]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "common/png.h"
#include "hle/xg/gcn.h"

int main(int argc, char** argv) {
    if (argc < 4) return std::fprintf(stderr, "usage: swatch_view file out.png bc4|r8 [tile]\n"), 2;
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) return 1;
    std::vector<uint8_t> d;
    uint8_t buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) d.insert(d.end(), buf, buf + n);
    std::fclose(f);
    uint32_t data_off, w, h, mips;
    std::memcpy(&data_off, &d[0x1c], 4);
    w = d[0x40] | d[0x41] << 8, h = d[0x42] | d[0x43] << 8, mips = d[0x4c];
    bool bc4 = !std::strcmp(argv[3], "bc4");
    fl::gcn::SurfaceDesc s;
    s.width = w, s.height = h, s.mips = mips, s.bytes_per_element = bc4 ? 8 : 1, s.block = bc4 ? 4 : 1;
    s.tile_index = argc > 4 ? std::atoi(argv[4]) : 14;
    fl::gcn::Layout l;
    if (!fl::gcn::compute_layout(s, &l)) return 1;
    std::printf("%ux%u mips %u -> layout size %llu, file data %zu\n", w, h, mips, (unsigned long long)l.size,
                d.size() - data_off);
    const auto& m = l.mips[0];
    std::vector<uint8_t> lin(size_t(m.width) * m.height * s.bytes_per_element);
    fl::gcn::untile(s, l, 0, 0, d.data() + data_off, lin.data(), size_t(m.width) * s.bytes_per_element);
    std::vector<uint8_t> rgba(size_t(w) * h * 4, 255);
    for (uint32_t by = 0; by < m.height; by++)
        for (uint32_t bx = 0; bx < m.width; bx++) {
            if (!bc4) {
                uint8_t v = lin[size_t(by) * m.width + bx];
                uint8_t* p = &rgba[(size_t(by) * w + bx) * 4];
                p[0] = p[1] = p[2] = v;
                continue;
            }
            const uint8_t* b = &lin[(size_t(by) * m.width + bx) * 8];
            uint8_t pal[8] = {b[0], b[1]};
            if (b[0] > b[1])
                for (int i = 1; i < 7; i++) pal[i + 1] = ((7 - i) * b[0] + i * b[1]) / 7;
            else {
                for (int i = 1; i < 5; i++) pal[i + 1] = ((5 - i) * b[0] + i * b[1]) / 5;
                pal[6] = 0, pal[7] = 255;
            }
            uint64_t bits = 0;
            std::memcpy(&bits, b + 2, 6);
            for (int i = 0; i < 16; i++) {
                uint8_t v = pal[(bits >> (3 * i)) & 7];
                uint8_t* p = &rgba[((size_t(by) * 4 + i / 4) * w + bx * 4 + i % 4) * 4];
                p[0] = p[1] = p[2] = v;
            }
        }
    return fl::write_png(argv[2], rgba.data(), w, h) ? 0 : 1;
}
