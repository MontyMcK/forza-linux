// Stand-in for DXVK in the PS5 runtime until DXVK is built for the PS5
// (PS5_PORT.md Step 5). Linked only by `tools/ps5/build.sh runtime` while no
// PS5 DXVK exists. Fails loudly (CLAUDE.md Rule 3): device creation returns an
// error, so the game stops at its first D3D call; enough for M1 (load the exe,
// resolve imports, log).
#include <cstdint>

#include "common/log.h"

extern "C" {

int32_t D3D11CreateDevice(void*, int, void*, uint32_t, const int*, uint32_t, uint32_t, void** device, int*, void** context) {
    FL_ERROR("UNIMPLEMENTED D3D11CreateDevice on the PS5: DXVK isn't built for the PS5 yet (PS5_PORT.md Step 5)");
    if (device) *device = nullptr;
    if (context) *context = nullptr;
    return static_cast<int32_t>(0x80004005);  // E_FAIL
}

void dxvk_xbox_stat_counters(void*, uint64_t* counters, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) counters[i] = 0;
}

void dxvk_xbox_remap_inheritance(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {
    FL_ERROR("UNIMPLEMENTED dxvk_xbox_remap_inheritance on the PS5: no DXVK yet");
}

}  // extern "C"
