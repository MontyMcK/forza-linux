// FL_GPU_PROFILE=<frame>[,<frame>...] or FL_GPU_PROFILE_AT_TIME=<s>[,<s>...]:
// GPU time per pass of those frames (or of the first frame after those run times).
// A timestamp query goes before every draw, dispatch, copy, clear and command
// list on the immediate context; after the frame the time between two
// timestamps is charged to the render/depth target (or compute) of the
// operation that started it, and the totals are logged (channel "gpuprof").
#pragma once

#include <cstdint>

namespace fl::d3d::gpuprof {

bool active();                               // a profiled frame is being recorded
void on_present(void* dxctx, uint64_t frame);  // frame = the number of the frame now starting
void mark(void* dxctx, const char* kind);    // before an operation on the immediate context

}  // namespace fl::d3d::gpuprof
