// Calling into dxvk-native.
//
// DXVK's COM objects use the Linux (System V) calling convention and the
// DESKTOP slot order (desktop_slots.h). The game's D3D11.X objects use ms_abi
// and the XBOX slot order (FINDINGS §3), so the two never mix: every object the
// game sees is one of our wrappers, and every DXVK pointer stays on our side.
//
// We don't include DXVK's Windows headers here (they clash with winabi.h).
// The D3D11 descriptor structs contain only 32-bit ints, floats and pointers,
// so the game's structs can be passed to DXVK by pointer unchanged.
#pragma once

#include "common/winabi.h"
#include "hle/d3d11x/desktop_slots.h"

namespace fl::d3d {

// Call slot `n` (desktop numbering) of a DXVK object.
template <typename R, typename... A>
R dx(void* obj, int n, A... args) {
    auto** vtbl = *static_cast<void***>(obj);
    return reinterpret_cast<R (*)(void*, A...)>(vtbl[n])(obj, args...);
}

inline HRESULT dx_query(void* obj, const GUID& iid, void** out) { return dx<HRESULT>(obj, 0, &iid, out); }
inline ULONG dx_release(void* obj) { return dx<ULONG>(obj, 2); }

// Xbox-only IIDs (names are ours). Each one is answered with the object itself
// by umd.dll's QueryInterface (FINDINGS §3).
// Device: umd device QI (rva 0x28627). The game QIs for it right after creation.
constexpr GUID IID_ID3D11DeviceX = {0x88671610, 0x712E, 0x4F1E, {0x84, 0xAB, 0x01, 0xB5, 0x94, 0x8B, 0xD3, 0x73}};
// Context: umd context QI (rva 0x8c33), checked first, so probably ID3D11DeviceContextX.
constexpr GUID IID_ID3D11DeviceContextX = {0x9458FE06, 0xC78D, 0x47F7, {0x96, 0xA0, 0xEC, 0x7B, 0x72, 0x7B, 0xE1, 0xE9}};
// Context: umd context QI (rva 0x8c62), checked second. Identity unknown.
constexpr GUID IID_XboxContextB2DAAD8B = {0xB2DAAD8B, 0x03D4, 0x4DBF, {0x95, 0xEB, 0x32, 0xAB, 0x4B, 0x63, 0xD0, 0xAB}};
// Context: umd generic QI (rva 0x336f). Identity unknown.
constexpr GUID IID_XboxContextBase48800095 = {0x48800095, 0x7134, 0x4BE7, {0x91, 0x86, 0xB8, 0x6B, 0xEC, 0xB2, 0x64, 0x77}};

// D3D_FEATURE_LEVEL values.
constexpr int kFeatureLevel11_0 = 0xb000;
constexpr int kFeatureLevel11_1 = 0xb100;

}  // namespace fl::d3d

// dxvk-native's exported D3D11CreateDevice (System V ABI on Linux).
extern "C" HRESULT D3D11CreateDevice(void* adapter, int driver_type, void* software, UINT flags,
                                     const int* feature_levels, UINT num_levels, UINT sdk_version,
                                     void** device, int* feature_level, void** context);
