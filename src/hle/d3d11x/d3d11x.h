// D3D11.X device, immediate context and device children, backed by dxvk-native.
//
// Every object the game sees is one of our wrappers (ms_abi, Xbox slot order);
// every DXVK object stays on our side (sysv, desktop slot order, see dxvk.h).
// Layouts and slot numbers: FINDINGS §3.1. Only fill a slot after checking it
// against umd.dll AND a game call site: Xbox signatures can differ from desktop.
#pragma once

#include <cstddef>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "hle/winrt/object.h"
#include "hle/xg/gcn.h"

namespace fl::d3d {

constexpr int kDeviceSlots = 84;
constexpr int kDxgiDeviceSlots = 18;

// ---- Xbox slot numbers from desktop ones (FINDINGS §3.1) ----
// Device: one extra method after SetPrivateDataInterface (desktop 36).
constexpr int dev_slot(int d) { return d >= 37 ? d + 1 : d; }
// IDXGIObject-based interfaces: extra method after SetPrivateDataInterface (desktop 4).
constexpr int dxgi_slot(int d) { return d >= 5 ? d + 1 : d; }
// Context: extra base method at 7, and no IASetPrimitiveTopology (desktop 24;
// the game always inlines it). So desktop 7..23 -> +1, desktop 25.. -> same.
constexpr int ctx_slot(int d) { return d >= 25 ? d : (d >= 7 ? d + 1 : d); }
// Device children: 0..6 like desktop, 7 = Xbox extra, then the interface's own methods.
constexpr int kChildSlots = 8;

constexpr HRESULT DXGI_ERROR_NOT_FOUND = static_cast<HRESULT>(0x887A0002);
constexpr HRESULT DXGI_ERROR_MORE_DATA = static_cast<HRESULT>(0x887A0003);
constexpr HRESULT DXGI_ERROR_UNSUPPORTED = static_cast<HRESULT>(0x887A0004);

// Per-object private data (debug names etc.).
class PrivateData {
public:
    ~PrivateData();
    HRESULT get(const GUID& g, UINT* size, void* data);
    HRESULT set(const GUID& g, UINT size, const void* data);
    HRESULT set_interface(const GUID& g, void* unknown);
    std::string name();  // WKPDID_D3DDebugObjectName, or ""

private:
    struct Entry {
        std::vector<uint8_t> bytes;
        void* iface = nullptr;
    };
    std::mutex m_;
    std::map<std::string, Entry> entries_;
};

struct DeviceX;

// Base of every wrapper the game can pass back to us as an ID3D11DeviceChild.
struct DeviceChild : winrt::Object {
    static constexpr uint32_t kMagic = 0x43443344;  // "D3DC"
    static constexpr uint32_t kDeadMagic = 0x44414544;  // "DEAD": released under FL_D3D_QUARANTINE
    DeviceChild(const char16_t* name, DeviceX* dev, winrt::Vtable& vt, void* dxvk_obj);
    ~DeviceChild() override;
    void destroy() override;

    // The resource whose memory this object reads (resources: themselves,
    // views: their resource), for placement-memory syncing.
    virtual struct Resource* backing() { return nullptr; }

    uint32_t magic = kMagic;
    DeviceX* device;
    void* dxvk;           // the DXVK object (one reference held)
    winrt::Iface iface;   // what the game holds
    PrivateData priv;
};

// The wrapper for a DXVK object, or nullptr.
DeviceChild* wrapper_of(void* dxvk_obj);
// Game interface pointer -> DXVK object (nullptr stays nullptr). Aborts on foreign objects.
void* unwrap(const void* game_iface);
DeviceChild* child_of(const void* game_iface);

// Slots 0..6 shared by every child vtable.
winrt::Vtable& child_base(winrt::Vtable& vt);
WINAPI HRESULT d3d_QueryInterface(winrt::Iface* self, const GUID& iid, void** out);

// ---- Resources ----
struct Resource : DeviceChild {
    Resource(const char16_t* name, DeviceX* dev, winrt::Vtable& vt, void* dxvk_obj, int dim, const void* xdesc,
             size_t xdesc_size);
    ~Resource() override;
    Resource* backing() override { return this; }
    int dim;                 // D3D11_RESOURCE_DIMENSION: 1 buffer, 2 tex1d, 3 tex2d, 4 tex3d
    uint8_t xdesc[0x40] = {};  // the game's Xbox descriptor (returned by GetDesc)
    size_t xdesc_size;

    // Placement resources (device slots 62-65) live in game memory.
    uint8_t* place = nullptr;
    size_t place_size = 0;     // bytes of game memory the resource covers (0 = unknown)
    uint64_t upload_hash = 0;  // FL_CHECK_TRACKING: hash of the last upload
    uint32_t tile_mode = 0;    // index into the GCN tile mode table (textures)
    uint32_t pitch = 0;        // in elements, 0 = default
    bool gpu_owned = false;    // render target / depth / UAV: the GPU writes it, never upload
    gcn::SurfaceDesc tex_desc; // placement textures: how the game memory is laid out
    gcn::Layout tex_layout;
    const uint32_t* desc32() const { return reinterpret_cast<const uint32_t*>(xdesc); }
};

// Create a buffer (dim 1) or texture (2-4) from an Xbox descriptor.
HRESULT create_resource(DeviceX* dev, int dim, const uint32_t* xdesc, const void* init, void** out, bool placement,
                        Resource** created);

winrt::Vtable& texture2d_vtable();          // resources.cpp
void* make_adapter(void* dxvk_adapter);      // dxgi.cpp: wraps an IDXGIAdapter

// Copy a placement resource's game memory to DXVK if it changed. Called when
// the resource is bound. Cheap when nothing changed.
void sync_placement(void* dxvk_ctx, Resource* r);
// A placement resource is going away: forget it as the owner of its address.
void placement_forget(Resource* r);
inline void sync_for_gpu(void* dxvk_ctx, DeviceChild* c) {
    if (Resource* r = c ? c->backing() : nullptr)
        if (r->place) sync_placement(dxvk_ctx, r);
}

// ---- The immediate context ----
// Game code INLINES many context methods: it reads and writes this block at
// fixed offsets, so the block must use the Xbox layout. The game passes the
// block pointer back to us as `this` in every method call.
struct ContextX {
    static constexpr size_t kBlockSize = 0x40000;  // real size unknown; umd touches +0x2b0c0
    static constexpr size_t kTable = 0x3d0;        // embedded method table (vptr points here)
    static constexpr int kSlots = 261;             // umd copies 0x828 bytes of table
    // Byte; bit 0: inlined raw command-buffer writes become slot 209 commands,
    // bit 1: inlined state setters call through the table.
    static constexpr size_t kModeFlags = 0x390;
    static constexpr size_t kDirtyFlags = 0x168;   // u32 the inlined fast paths OR into
    static constexpr size_t kTopology = 0x170;     // u64 written by the inlined IASetPrimitiveTopology
    static constexpr size_t kDevicePtr = 0x10;     // device interface pointer (as umd's AddRef reads it)

    alignas(64) uint8_t block[kBlockSize];
    DeviceX* device = nullptr;
    void* dxvk = nullptr;  // DXVK ID3D11DeviceContext
    uint64_t applied_topology_bits = 0;
    // The immediate context shares the device's refcount (as in umd); deferred
    // contexts (same layout, umd table 0x117b60) have their own.
    bool deferred = false;
    std::atomic<ULONG> refs{1};
    bool slot196_flag = false;

    static ContextX* from(void* self) { return reinterpret_cast<ContextX*>(self); }
    void** table() { return reinterpret_cast<void**>(block + kTable); }
    uint64_t& u64(size_t off) { return *reinterpret_cast<uint64_t*>(block + off); }
};
static_assert(offsetof(ContextX, block) == 0);

struct DeviceX : winrt::Object {
    DeviceX();
    ~DeviceX() override;

    winrt::Iface device_iface;  // ID3D11DeviceX (Xbox slot order)
    winrt::Iface dxgi_iface;    // IDXGIDevice1/2 (Xbox slot order)
    ContextX* immediate = nullptr;
    UINT creation_flags = 0;
    void* dxvk = nullptr;       // DXVK ID3D11Device
    PrivateData priv;
};

// Slot fillers implemented in the other files.
void add_resource_methods(winrt::Vtable& device_vt);  // resources.cpp
void add_pipeline_methods(winrt::Vtable& device_vt);  // pipeline.cpp
void add_dma_methods(winrt::Vtable& device_vt);       // dma.cpp
void add_placement_methods(winrt::Vtable& device_vt); // placement.cpp
void add_context_methods(void** table);               // context.cpp
ContextX* make_context(DeviceX* d, void* dxvk_ctx, bool deferred);  // context.cpp

// FL_DEBUG_NULL_SRV support: the bytecode hash (FNV-1a) of a DXVK shader.
void note_shader_hash(void* dxvk_shader, uint64_t hash);
uint64_t shader_hash(void* dxvk_shader);

// Log a value once per (what, value) pair.
void log_once(const char* what, uint64_t value);

}  // namespace fl::d3d
