#include "hle/xg/formats.h"

#include "common/log.h"

namespace fl::xg {
namespace {

struct Entry {
    uint32_t first, last;
    FormatInfo info;
};

// Ranges of public DXGI_FORMAT values (dxgiformat.h).
constexpr Entry kFormats[] = {
    {1, 4, {16}},            // R32G32B32A32_*
    {5, 8, {12}},            // R32G32B32_*
    {9, 14, {8}},            // R16G16B16A16_*
    {15, 18, {8}},           // R32G32_*
    {19, 19, {8, 1, true, true}},  // R32G8X24_TYPELESS
    {20, 20, {8, 1, true, true}},  // D32_FLOAT_S8X24_UINT
    {21, 22, {8}},           // R32_FLOAT_X8X24_TYPELESS, X32_TYPELESS_G8X24_UINT
    {23, 38, {4}},           // R10G10B10A2_*, R11G11B10_FLOAT, R8G8B8A8_*, R16G16_*
    {39, 39, {4}},           // R32_TYPELESS
    {40, 40, {4, 1, true}},  // D32_FLOAT
    {41, 43, {4}},           // R32_*
    {44, 44, {4, 1, true, true}},  // R24G8_TYPELESS
    {45, 45, {4, 1, true, true}},  // D24_UNORM_S8_UINT
    {46, 47, {4}},           // R24_UNORM_X8_TYPELESS, X24_TYPELESS_G8_UINT
    {48, 52, {2}},           // R8G8_*
    {53, 54, {2}},           // R16_TYPELESS, R16_FLOAT
    {55, 55, {2, 1, true}},  // D16_UNORM
    {56, 59, {2}},           // R16_*
    {60, 65, {1}},           // R8_*, A8_UNORM
    {67, 69, {4}},           // R9G9B9E5_SHAREDEXP, R8G8_B8G8, G8R8_G8B8
    {70, 72, {8, 4}},        // BC1
    {73, 78, {16, 4}},       // BC2, BC3
    {79, 81, {8, 4}},        // BC4
    {82, 84, {16, 4}},       // BC5
    {85, 86, {2}},           // B5G6R5, B5G5R5A1
    {87, 93, {4}},           // B8G8R8A8/X8 variants, R10G10B10_XR_BIAS_A2
    {94, 99, {16, 4}},       // BC6H, BC7
    {115, 115, {2}},         // B4G4R4A4_UNORM
};

}  // namespace

const FormatInfo& format_info(uint32_t fmt) {
    for (const Entry& e : kFormats)
        if (fmt >= e.first && fmt <= e.last) return e.info;
    fatal("xg: unknown DXGI format %u (add it to src/hle/xg/formats.cpp with evidence)", fmt);
}

}  // namespace fl::xg
