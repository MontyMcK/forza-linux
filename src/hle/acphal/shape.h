// SHAPE: the Xbox One audio block FMOD drives through AcpHal. Our software
// version executes the flowgraphs FMOD submits: XMA voices are decoded (or
// consumed as silence), resampled by their SRC contexts into 128-sample mix
// buffers, and DMA contexts copy mix buffers into the rings FMOD reads.
// Layouts and evidence: FINDINGS §4.
#pragma once

#include <cstdint>
#include <functional>

namespace fl::acphal {

// What acphal.cpp owns and SHAPE reads.
struct ShapeArrays {
    uint8_t* src = nullptr;  // 32-byte SRC contexts (index = XMA/PCM index)
    uint8_t* dma = nullptr;  // 16-byte DMA contexts
    uint8_t* xma = nullptr;  // 64-byte XMA contexts
    uint32_t num_src = 0, num_dma = 0, num_xma = 0;
};
const ShapeArrays& shape_arrays();
// APU (device) address -> our pointer, or nullptr if [apu, apu + len) is not in an APU heap.
uint8_t* apu_va(uint32_t apu, uint32_t len);

namespace shape {

// A message for FMOD (24 bytes as IAcpHal::PopMessage returns it).
struct Message {
    uint32_t type;
    uint32_t data[5];
};
using Post = std::function<void(const Message&)>;

// Commands that change SHAPE state (AcpHal command types):
// 5/6 start XMA contexts (one index / 512-bit mask), 7/8 stop them,
// 0x10 DMA consumption {u32 dma, u32 blocks}, 0x13 XMA input update
// {u32 xma, u32 buffer, u32 packets, u32 set_packets, u32 set_ptr, u32 apu}.
// Returns false for a type SHAPE does not handle.
bool command(uint32_t type, const uint8_t* payload);

// Runs one flowgraph (`count` 64-bit opcodes `ops`, copied from APU address
// `apu` when the command was submitted; nullptr if that was outside the APU
// heaps) for one audio frame. Posts message 4 for a starved SRC voice and
// message 2 when done.
void run_graph(const uint64_t* ops, uint32_t count, uint32_t apu, const Post& post);

// The frame counter stamped into SRC/DMA contexts (increments per graph).
uint32_t frame();

void reset();

}  // namespace shape
}  // namespace fl::acphal
