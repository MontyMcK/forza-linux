// Device slot 59: the Xbox-only object the game creates twice right after the
// device (FINDINGS §3.1). Probably a DMA/copy-engine context: its methods take
// GPU virtual addresses, submit work and wait on fences. umd: 30-slot vtable at
// 0x16f580. Only slots verified at a game call site are filled.
#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>
#include <sys/uio.h>
#include <unistd.h>
#include <zlib.h>

#include "common/log.h"
#include "hle/d3d11x/d3d11x.h"
#include "hle/d3d11x/dxvk.h"
#include "hle/d3d11x/trace.h"
#include "platform/os/memory.h"

namespace fl::d3d {
namespace {

using winrt::Iface;
using winrt::Vtable;

struct DmaDesc {
    UINT flags;      // low 4 bits: engine (the game passes 1 and 2)
    UINT ring_size;  // 0 means 0x80000 in umd
    UINT unknown8;
};

struct DmaContext : DeviceChild {
    DmaContext(DeviceX* d, Vtable& vt, const DmaDesc& desc) : DeviceChild(u"D3D11DmaContextX", d, vt, nullptr), desc(desc) {}
    DmaDesc desc;
    std::atomic<uint32_t> status{0};  // HRESULT of the last LZDecompressMemory (slot 19 writes it)
};

DmaContext* dma_of(Iface* self) { return static_cast<DmaContext*>(child_of(self)); }

// umd returns the constant 2 here; meaning unknown (maybe a context type).
WINAPI UINT dma_Slot8(Iface* self) { return 2; }

// Resource copies (Azimuth fills exactly 9, 10, 16-19, 22 and 24 for this
// interface, so those are what the game uses). On the console the DMA engine
// copies GPU memory; here the resources live in DXVK, so the copy is recorded
// on the immediate DXVK context (DXVK's multithread lock is on, FINDINGS §5b).
// Recording it now orders it after the graphics work the game told the DMA
// engine to wait for (17) and before graphics work that waits on this engine.
void* dma_dxvk_context(Iface* self) { return dma_of(self)->device->immediate->dxvk; }

// 9: CopyResource(dst, src, flags); flags must be 0 (Azimuth: "DMA CopyResource
// flags="). Game rva 0x10ced4b.
WINAPI void dma_CopyResource(Iface* self, void* dst, void* src, UINT flags) {
    FL_LOG(dma, "CopyResource %p <- %p", dst, src);
    if (flags) log_once("DMA CopyResource: unknown flags", flags);
    void* ctx = dma_dxvk_context(self);
    DeviceChild* s = child_of(src);
    sync_for_gpu(ctx, s);
    trace::note_write("DMA CopyResource", unwrap(dst), s->dxvk);
    dx<void>(ctx, desktop::ctx::CopyResource, unwrap(dst), s->dxvk);
}
// 10: CopySubresourceRegion(dst, dst_sub, x, y, z, src, src_sub, box, flags):
// the desktop arguments plus flags (Azimuth: "DMA CopySubresourceRegion flags=").
WINAPI void dma_CopySubresourceRegion(Iface* self, void* dst, UINT dst_sub, UINT x, UINT y, UINT z, void* src,
                                      UINT src_sub, const void* box, UINT flags) {
    FL_LOG(dma, "CopySubresourceRegion %p sub %u <- %p sub %u", dst, dst_sub, src, src_sub);
    if (flags) log_once("DMA CopySubresourceRegion: unknown flags", flags);
    void* ctx = dma_dxvk_context(self);
    DeviceChild* s = child_of(src);
    sync_for_gpu(ctx, s);
    trace::note_write("DMA CopySubresourceRegion", unwrap(dst), s->dxvk);
    dx<void>(ctx, desktop::ctx::CopySubresourceRegion, unwrap(dst), dst_sub, x, y, z, s->dxvk, src_sub, box);
}

// We run DMA work synchronously on the CPU: on the console the GPU address of
// title memory is its CPU address, so copies and fills are plain memory
// operations (writes into placement memory are seen by gpumem's write watch),
// and every DMA fence has already passed when it is waited on.
//
// 16 (umd 0x8f8d0): InsertFence(flags) -> u64 fence; umd also kicks off the
// recorded work unless flags bit 0 is set. Our work is already done, so the
// value only has to be non-zero and increasing.
WINAPI uint64_t dma_InsertFence(Iface* self, UINT flags) {
    static std::atomic<uint64_t> fence{0};
    uint64_t v = ++fence;
    FL_LOG(fence, "dma %p InsertFence(0x%x) -> %llu", (void*)self, flags, static_cast<unsigned long long>(v));
    return v;
}
// 17 (umd 0x8f910): InsertWaitOnFence(flags, u64 fence); fence 0 = nothing,
// bits 48-55 name the engine. The game waits on a graphics fence (rva 0x1131b74).
WINAPI void dma_InsertWaitOnFence(Iface* self, UINT flags, uint64_t fence) {
    FL_LOG(fence, "dma %p InsertWaitOnFence(0x%x, 0x%llx)", (void*)self, flags, static_cast<unsigned long long>(fence));
}
// 18 (umd 0x8f960): kick off the recorded work; HRESULT.
WINAPI HRESULT dma_Kickoff(Iface* self) {
    FL_LOG(fence, "dma %p Kickoff", (void*)self);
    return S_OK;
}
// Writes are checked so a destination the CPU never committed (GPU-only
// graphics memory reserved with the Xbox type bit 0x40000000, whose contents
// live in DXVK for us) is skipped instead of faulting.
bool dma_write(uint64_t dst, const void* src, size_t n) {
    if (os::write_checked(reinterpret_cast<void*>(dst), src, n) == n) return true;
    FL_STUB_ONCE("DMA: write into memory the CPU cannot write (GPU-only, not committed): skipped");
    return false;
}

// 21 (umd 0x8fa50): CopyMemoryToMemory(dst, src, bytes); umd ignores the call
// for addresses >= 1 << 40 or 0 bytes.
WINAPI void dma_CopyMemoryToMemory(Iface*, uint64_t dst, uint64_t src, uint64_t bytes) {
    FL_LOG(dma, "CopyMemoryToMemory %p <- %p, 0x%llx bytes", reinterpret_cast<void*>(dst), reinterpret_cast<void*>(src),
           (unsigned long long)bytes);
    if (!bytes) return;
    std::vector<uint8_t> tmp(std::min<uint64_t>(bytes, 1 << 20));
    for (uint64_t off = 0; off < bytes; off += tmp.size()) {
        size_t n = static_cast<size_t>(std::min<uint64_t>(tmp.size(), bytes - off));
        if (os::read_checked(tmp.data(), reinterpret_cast<void*>(src + off), n) != n) {
            FL_STUB_ONCE("DMA: copy from memory the CPU cannot read (GPU-only): skipped");
            return;
        }
        if (!dma_write(dst + off, tmp.data(), n)) return;
    }
}
// 22 (umd 0x8fa80): FillMemoryWithValue(dst, bytes, u32 value), in dwords.
// A fill over a GPU-owned placement texture (render/depth target): its
// contents live in DXVK, so the fill must happen there. The game clears its
// placement shadow maps this way every frame: HTILE with 0xffffffff, then the
// depth plane with 1.0f. Returns true when the fill was turned into a GPU clear.
bool fill_gpu_owned(Iface* self, uint64_t dst, uint64_t bytes, uint32_t value) {
    Resource* r = placement_containing(dst);
    if (!r) return false;
    struct Release {
        Resource* r;
        ~Release() { r->release(); }
    } hold{r};
    if (!r->gpu_owned) return false;
    uint64_t off = dst - reinterpret_cast<uint64_t>(r->place);
    const xg::PlaneSpan* p = nullptr;
    for (int i = 0; i < r->num_planes; i++)
        if (off >= r->planes[i].offset && off < r->planes[i].offset + r->planes[i].size) p = &r->planes[i];
    if (!p) {
        log_once("UNIMPLEMENTED DMA fill inside a GPU-owned resource outside its planes", static_cast<uint32_t>(off));
        return true;
    }
    if (p->usage == 4 /* HTILE */) return true;  // we keep no HTILE: nothing to clear
    if (p->usage != 7 /* depth */ && p->usage != 8 /* stencil */) {
        log_once("UNIMPLEMENTED DMA fill of a GPU-owned plane, usage", p->usage);
        return true;
    }
    if (off != p->offset || bytes < p->size) {
        log_once("UNIMPLEMENTED DMA fill of part of a depth/stencil plane, bytes", static_cast<uint32_t>(bytes));
        return true;
    }
    const uint32_t* d = r->desc32();  // D3D11_TEXTURE2D_DESC
    if (r->dim != 3) return false;
    uint32_t fmt = d[4], dsv_fmt = 0;
    switch (fmt) {
        case 39: case 40: dsv_fmt = 40; break;  // R32_TYPELESS / D32_FLOAT
        case 19: case 20: dsv_fmt = 20; break;  // R32G8X24_TYPELESS / D32_FLOAT_S8X24_UINT
        case 44: case 45: dsv_fmt = 45; break;  // R24G8_TYPELESS / D24_UNORM_S8_UINT
        case 53: case 55: dsv_fmt = 55; break;  // R16_TYPELESS / D16_UNORM
    }
    if (!dsv_fmt || (p->usage == 7 && dsv_fmt != 40 && dsv_fmt != 20)) {
        log_once("UNIMPLEMENTED DMA depth/stencil fill for format", fmt);
        return true;
    }
    // D3D11_DEPTH_STENCIL_VIEW_DESC: format, dimension, flags, then the union.
    uint32_t vd[6] = {dsv_fmt, d[5] > 1 ? 6u : 4u, 0, 0, 0, d[3]};  // TEXTURE2D(MS)ARRAY, all slices
    if (d[5] > 1) vd[3] = 0, vd[4] = d[3];                             // MS array: first slice, count
    void* dsv = nullptr;
    HRESULT hr = dx<HRESULT>(r->device->dxvk, desktop::dev::CreateDepthStencilView, r->dxvk, vd, &dsv);
    if (hr < 0 || !dsv) {
        FL_ERROR("DMA fill: depth view of [%p fmt %u] failed: 0x%08x", (void*)r, fmt, static_cast<uint32_t>(hr));
        return true;
    }
    float depth;
    std::memcpy(&depth, &value, sizeof depth);
    UINT flags = p->usage == 7 ? 1u /* CLEAR_DEPTH */ : 2u /* CLEAR_STENCIL */;
    FL_LOG(dma, "  -> GPU clear of [%p %ux%u fmt %u] %s %g / 0x%x", (void*)r, d[0], d[1], fmt,
           flags == 1 ? "depth" : "stencil", depth, value & 0xff);
    dx<void>(dma_dxvk_context(self), desktop::ctx::ClearDepthStencilView, dsv, flags, depth,
             static_cast<uint8_t>(value & 0xff));
    dx_release(dsv);
    return true;
}

WINAPI void dma_FillMemoryWithValue(Iface* self, uint64_t dst, uint64_t bytes, uint32_t value) {
    FL_LOG(dma, "FillMemoryWithValue %p, 0x%llx bytes, 0x%x", reinterpret_cast<void*>(dst), (unsigned long long)bytes, value);
    if (fill_gpu_owned(self, dst, bytes, value)) return;
    std::vector<uint32_t> pattern(std::min<uint64_t>(bytes / 4, 1 << 18), value);
    uint64_t total = bytes & ~uint64_t(3);
    for (uint64_t off = 0; off < total; off += pattern.size() * 4) {
        size_t n = static_cast<size_t>(std::min<uint64_t>(pattern.size() * 4, total - off));
        if (!dma_write(dst + off, pattern.data(), n)) return;
    }
}

// 19 (umd 0x8f9c0): write the engine's 4-byte status to `dst` (4-aligned).
// Azimuth ("DMA status") writes the HRESULT of the last LZDecompressMemory.
WINAPI void dma_WriteStatus(Iface* self, uint64_t dst) {
    uint32_t v = dma_of(self)->status.load();
    FL_LOG(dma, "WriteStatus %p = 0x%x", reinterpret_cast<void*>(dst), v);
    dma_write(dst, &v, sizeof v);
}

// 24 (umd 0x8fae0): LZDecompressMemory(dst, src, src_bytes, flags): the source
// is a zlib stream (RFC 1950: header, deflate data, big-endian Adler-32), as
// Azimuth's decoder checks; the output length comes from the stream. umd
// rejects 0 bytes and addresses >= 1 << 40; Azimuth saw only flags 0 and sizes
// below 64 MiB. Status: 0, or 0x8007000D bad data / 0x80070017 bad checksum.
WINAPI HRESULT dma_LZDecompressMemory(Iface* self, uint64_t dst, uint64_t src, UINT src_bytes, UINT flags) {
    if (flags) log_once("DMA LZDecompressMemory: unknown flags", flags);
    if (!src_bytes) return 0x80070057;  // E_INVALIDARG, as umd
    std::vector<uint8_t> in(src_bytes);
    if (os::read_checked(in.data(), reinterpret_cast<void*>(src), in.size()) != in.size()) {
        FL_ERROR("DMA LZDecompressMemory: source %p+0x%x is not readable", reinterpret_cast<void*>(src), src_bytes);
        dma_of(self)->status = 0x8007000D;
        return S_OK;
    }
    std::vector<uint8_t> out(std::max<size_t>(src_bytes * 4u, 1 << 16));
    z_stream z{};
    inflateInit(&z);
    z.next_in = in.data();
    z.avail_in = src_bytes;
    int r = Z_OK;
    while (r == Z_OK) {
        if (z.total_out == out.size()) out.resize(out.size() * 2);
        z.next_out = out.data() + z.total_out;
        z.avail_out = static_cast<uInt>(out.size() - z.total_out);
        r = inflate(&z, Z_NO_FLUSH);
        if (r == Z_BUF_ERROR && z.avail_out == 0) r = Z_OK;  // just needs more room
    }
    size_t n = z.total_out;
    inflateEnd(&z);
    uint32_t status = r == Z_STREAM_END ? 0 : r == Z_DATA_ERROR && z.msg && strstr(z.msg, "check") ? 0x80070017 : 0x8007000D;
    FL_LOG(dma, "LZDecompressMemory %p <- %p (0x%x bytes) -> 0x%zx bytes, status 0x%x", reinterpret_cast<void*>(dst),
           reinterpret_cast<void*>(src), src_bytes, n, status);
    if (status) FL_ERROR("DMA LZDecompressMemory: bad stream at %p (zlib %d)", reinterpret_cast<void*>(src), r);
    else dma_write(dst, out.data(), n);
    dma_of(self)->status = status;
    return S_OK;
}

Vtable& dma_vt() {
    static Vtable v = [] {
        Vtable t("ID3D11DmaEngineContextX", 30, false);
        child_base(t)
            .set(8, &dma_Slot8)
            .set(9, &dma_CopyResource)
            .set(10, &dma_CopySubresourceRegion)
            .set(16, &dma_InsertFence)
            .set(17, &dma_InsertWaitOnFence)
            .set(18, &dma_Kickoff)
            .set(19, &dma_WriteStatus)
            .set(21, &dma_CopyMemoryToMemory)
            .set(22, &dma_FillMemoryWithValue)
            .set(24, &dma_LZDecompressMemory);
        return t;
    }();
    return v;
}

// Xbox device slot 59: (const DmaDesc*, out**). Game calls at rva 0x10d8afb / 0x10d8b3d.
WINAPI HRESULT dev_CreateDmaContext(Iface* self, const DmaDesc* desc, void** out) {
    FL_LOG(d3d, "CreateDmaContext(flags=0x%x ring=0x%x +8=0x%x)", desc->flags, desc->ring_size, desc->unknown8);
    auto* c = new DmaContext(self->as<DeviceX>(), dma_vt(), *desc);
    *out = &c->iface;
    return S_OK;
}

}  // namespace

void add_dma_methods(Vtable& v) { v.set(59, &dev_CreateDmaContext); }

}  // namespace fl::d3d
