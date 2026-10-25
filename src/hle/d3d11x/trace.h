// Frame trace for rendering bugs: FL_TRACE_FRAME=N[,M...] logs every draw,
// dispatch, copy, clear, map and placement upload of those frames (channel
// "trace", always printed while a traced frame is in progress). Frame numbers
// are the swap chain's (the same as FL_SCREENSHOT_AT_FRAME, counted from 1).
#pragma once

#include <cstdint>

namespace fl::d3d::trace {

void on_present(uint64_t frame);  // frame = the number of the frame now starting
bool active();

// A draw or dispatch on the DXVK context `dxctx`: logs it and the bound state.
void draw(void* dxctx, const char* kind, uint32_t a, uint32_t b = 0, uint32_t c = 0);
// After a draw/dispatch: with FL_TRACE_DUMP set, saves the render target 0
// (and, first time each is seen, the draw's 2D textures) as PNGs in
// screenshots/trace/.
void after_draw(void* dxctx);
// Any other event (printf-style).
void event(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// One-line description of a game resource/view interface or DXVK object.
const char* describe_dxvk(void* dxvk_obj);

}  // namespace fl::d3d::trace
