// AcpHal.dll: the Xbox One audio hardware layer — APU memory heaps, SHAPE
// context arrays and IAcpHal, the command/message queue to the audio control
// processor (ACP) firmware. FMOD (statically linked in the game) uses it for
// hardware XMA decoding; its mixing runs on the CPU and goes out via WASAPI.
//
// Our ACP is software: a thread ticking once per audio frame (128 samples at
// 48 kHz) that consumes submitted commands and posts messages back.
// Evidence and layouts: FINDINGS §4f.
//
// SHAPE flowgraphs (XMA decode -> SRC -> DMA into FMOD's rings) run in
// shape.cpp; XMA is decoded with FFmpeg when tools/build_ffmpeg_xma.sh has
// been run, otherwise consumed as silence (STUB).

#include <sys/mman.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "common/log.h"
#include "common/winabi.h"
#include "hle/acphal/shape.h"
#include "hle/registry.h"

namespace fl::acphal {
namespace {

// ---------------------------------------------------------------- APU heaps

constexpr uint32_t E_INVALIDARG_ = 0x80070057;
constexpr uint32_t E_OUTOFMEMORY_ = 0x8007000E;
constexpr uint32_t kApuHeapAlreadyCreated = 0x8ACA0001;

// One heap: a contiguous mapping with a fixed APU (32-bit device) address.
struct Heap {
    uint8_t* base = nullptr;
    uint32_t apu_base = 0;
    uint32_t size = 0;
    std::map<uint32_t, uint32_t> used;  // offset -> size

    bool contains(const void* va) const {
        auto* p = static_cast<const uint8_t*>(va);
        return base && p >= base && p < base + size;
    }
    bool contains_apu(uint32_t a) const { return base && a >= apu_base && a - apu_base < size; }

    // First fit with alignment. Returns the offset or UINT32_MAX.
    uint32_t alloc(uint32_t n, uint32_t align) {
        if (align == 0) align = 1;
        uint64_t pos = 0;
        for (auto& [off, sz] : used) {
            uint64_t a = (pos + align - 1) / align * align;
            if (a + n <= off) {
                used[uint32_t(a)] = n;
                return uint32_t(a);
            }
            pos = uint64_t(off) + sz;
        }
        uint64_t a = (pos + align - 1) / align * align;
        if (a + n > size) return UINT32_MAX;
        used[uint32_t(a)] = n;
        return uint32_t(a);
    }
};

std::mutex g_heap_lock;
uint32_t g_cached_size = 0x400000, g_noncached_size = 0x40000;  // defaults if ApuCreateHeap isn't called
Heap g_heaps[2];  // [0] cached (flags 1), [1] non-cached (flags 2)
bool g_heaps_ready = false;

void ensure_heaps() {
    if (g_heaps_ready) return;
    uint32_t sizes[2] = {g_cached_size, g_noncached_size};
    uint32_t apu = 0x10000000;  // arbitrary non-zero device address base
    for (int i = 0; i < 2; i++) {
        uint32_t sz = (sizes[i] + 0xFFFF) & ~0xFFFFu;
        void* p = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) fl::fatal("ApuCreateHeap: mmap(%u) failed", sz);
        g_heaps[i].base = static_cast<uint8_t*>(p);
        g_heaps[i].apu_base = apu;
        g_heaps[i].size = sz;
        apu += sz + 0x100000;
    }
    g_heaps_ready = true;
    FL_LOG(audio, "APU heaps: cached %u KB @apu 0x%x, non-cached %u KB @apu 0x%x", g_heaps[0].size >> 10,
           g_heaps[0].apu_base, g_heaps[1].size >> 10, g_heaps[1].apu_base);
}

Heap* heap_of(const void* va) {
    for (auto& h : g_heaps)
        if (h.contains(va)) return &h;
    return nullptr;
}

HRESULT heap_alloc(int which, void** va, uint32_t* apu, uint32_t size, uint32_t align) {
    ensure_heaps();
    Heap& h = g_heaps[which];
    uint32_t off = h.alloc(size ? size : 1, align);
    if (off == UINT32_MAX) {
        FL_ERROR("ApuAlloc: %s heap exhausted (%u bytes, align %u)", which ? "non-cached" : "cached", size, align);
        return E_OUTOFMEMORY_;
    }
    memset(h.base + off, 0, size);
    if (va) *va = h.base + off;
    if (apu) *apu = h.apu_base + off;
    return S_OK;
}

WINAPI HRESULT ApuCreateHeap_(uint32_t cached, uint32_t noncached) {
    std::lock_guard lock(g_heap_lock);
    if (!cached) return E_INVALIDARG_;
    if (g_heaps_ready) return kApuHeapAlreadyCreated;
    g_cached_size = cached;
    if (noncached) g_noncached_size = noncached;
    ensure_heaps();
    return S_OK;
}

// flags: 1 = cached heap, 2 = non-cached heap (AcpHal rva 0x24f0).
WINAPI HRESULT ApuAlloc_(void** va, uint32_t* apu, uint32_t size, uint32_t align, uint32_t flags) {
    if (flags != 1 && flags != 2) return E_INVALIDARG_;
    std::lock_guard lock(g_heap_lock);
    HRESULT hr = heap_alloc(flags - 1, va, apu, size, align);
    FL_LOG(audio, "ApuAlloc(%u, align %u, heap %u) -> %p", size, align, flags, va ? *va : nullptr);
    return hr;
}

WINAPI HRESULT ApuFree_(void* va) {
    std::lock_guard lock(g_heap_lock);
    Heap* h = heap_of(va);
    if (!h) return E_INVALIDARG_;
    auto it = h->used.find(uint32_t(static_cast<uint8_t*>(va) - h->base));
    if (it == h->used.end()) return E_INVALIDARG_;
    h->used.erase(it);
    return S_OK;
}

WINAPI uint32_t ApuMapVirtualAddress_(const void* va) {
    std::lock_guard lock(g_heap_lock);
    Heap* h = heap_of(va);
    return h ? h->apu_base + uint32_t(static_cast<const uint8_t*>(va) - h->base) : 0;
}

WINAPI void* ApuMapApuAddress_(uint32_t apu) {
    std::lock_guard lock(g_heap_lock);
    for (auto& h : g_heaps)
        if (h.contains_apu(apu)) return h.base + (apu - h.apu_base);
    return nullptr;
}

// True if [va] lies inside a live allocation. `access` (read/write bits) is
// not distinguished: all our APU memory is read/write.
WINAPI bool ApuIsVirtualAddressValid_(const void* va, uint32_t) {
    std::lock_guard lock(g_heap_lock);
    Heap* h = heap_of(va);
    if (!h) return false;
    uint32_t off = uint32_t(static_cast<const uint8_t*>(va) - h->base);
    auto it = h->used.upper_bound(off);
    if (it == h->used.begin()) return false;
    --it;
    return off < it->first + it->second;
}

// ---------------------------------------------------------------- SHAPE contexts

// SHAPE_CONTEXTS (0x60): six counts, six arrays, six APU addresses.
// Element sizes: SRC 32, EQ/comp 160, filter/volume 32, DMA 16, XMA 64, PCM 32.
// numSrc must equal numXma + numPcm. XMA and PCM share one allocation.
struct ShapeContexts {
    uint32_t num_src, num_eqcomp, num_filtvol, num_dma, num_xma, num_pcm;
    void *src, *eqcomp, *filtvol, *dma, *xma, *pcm;
    uint32_t apu_src, apu_eqcomp, apu_filtvol, apu_dma, apu_xma, apu_pcm;
};
static_assert(sizeof(ShapeContexts) == 0x60);

ShapeContexts g_contexts{};
bool g_contexts_allocated = false;
constexpr uint32_t kContextsAlreadyAllocated = 0x8AC80001;

WINAPI HRESULT AcpHalAllocateShapeContexts_(ShapeContexts* c) {
    std::lock_guard lock(g_heap_lock);
    if (g_contexts_allocated) return kContextsAlreadyAllocated;
    if (c->num_xma > 0x200 || c->num_src > 0x1000 || c->num_eqcomp > 0x1000 || c->num_filtvol > 0x1000 ||
        c->num_dma > 0x1000 || c->num_pcm > 0xE00)
        return E_INVALIDARG_;
    if ((c->num_src || c->num_pcm) && c->num_src != c->num_xma + c->num_pcm) return E_INVALIDARG_;
    HRESULT hr = S_OK;
    auto alloc = [&](uint32_t count, uint32_t elem, uint32_t align, void** va, uint32_t* apu) {
        if (count && hr >= 0) hr = heap_alloc(1, va, apu, count * elem, align);
    };
    alloc(c->num_src, 32, 0x20, &c->src, &c->apu_src);
    alloc(c->num_eqcomp, 160, 0x10, &c->eqcomp, &c->apu_eqcomp);
    alloc(c->num_filtvol, 32, 0x20, &c->filtvol, &c->apu_filtvol);
    alloc(c->num_dma, 16, 0x10, &c->dma, &c->apu_dma);
    if ((c->num_xma || c->num_pcm) && hr >= 0) {
        void* va;
        uint32_t apu;
        hr = heap_alloc(1, &va, &apu, (c->num_xma * 2 + c->num_pcm) * 32, 0x40);
        if (hr >= 0) {
            c->xma = c->num_xma ? va : nullptr;
            c->apu_xma = c->num_xma ? apu : 0;
            c->pcm = c->num_pcm ? static_cast<uint8_t*>(va) + c->num_xma * 64 : nullptr;
            c->apu_pcm = c->num_pcm ? apu + c->num_xma * 64 : 0;
        }
    }
    if (hr < 0) return hr;
    g_contexts = *c;
    g_contexts_allocated = true;
    FL_LOG(audio, "SHAPE contexts: src %u eq %u filt %u dma %u xma %u pcm %u", c->num_src, c->num_eqcomp,
           c->num_filtvol, c->num_dma, c->num_xma, c->num_pcm);
    return S_OK;
}

WINAPI HRESULT AcpHalReleaseShapeContexts_() {
    std::lock_guard lock(g_heap_lock);
    if (!g_contexts_allocated) return 0x8AC80002;
    for (void* p : {g_contexts.src, g_contexts.eqcomp, g_contexts.filtvol, g_contexts.dma,
                    g_contexts.xma ? g_contexts.xma : g_contexts.pcm})
        if (Heap* h = heap_of(p)) h->used.erase(uint32_t(static_cast<uint8_t*>(p) - h->base));
    g_contexts = {};
    g_contexts_allocated = false;
    return S_OK;
}

// ---------------------------------------------------------------- IAcpHal

// A message as PopMessage returns it (24 bytes): type bit, then type-specific.
struct AcpMessage {
    uint32_t type;
    uint32_t data[5];
};
static_assert(sizeof(AcpMessage) == 24);

// Message types seen in FMOD's handler (game rva 0x19d4f40): 2 = flowgraph
// completed (returns a submission credit; +8 flowgraph APU address), 4 = an
// SRC voice ran out of XMA input (+8 SRC index, +0xc flowgraph), 0x10 = a
// command with a non-zero context was processed (+8 command type, +0xc u64
// context, +0x14 frame counter), 8/0x20/0x40/0x80 = errors it logs.
constexpr uint32_t kMsgCommandDone = 0x10;

// Commands (SubmitCommand type 1..0x1b) and payload sizes (AcpHal rva 0x14a4):
// 1, 0x15..0x1b: 16 bytes; 2,3,5,7: 4; 4: none; 6,8: 64; 9..0xe: 12;
// 0xf..0x12: 8; 0x13: 24; 0x14: 112.
constexpr uint32_t kCmdFlowgraph = 1;    // {u32 numCommands, u32 flowgraphApu, u64 ?}
constexpr uint32_t kCmdMessageMask = 2;  // {u32 mask of message types to deliver}
constexpr uint32_t kCmdMessageUnmask = 3;  // {u32 mask of message types to stop delivering}

size_t payload_size(uint32_t type) {
    if (type == 1 || (type >= 0x15 && type <= 0x1b)) return 16;
    if (type == 2 || type == 3 || type == 5 || type == 7) return 4;
    if (type == 4) return 0;
    if (type == 6 || type == 8) return 64;
    if (type >= 9 && type <= 0xe) return 12;
    if (type >= 0xf && type <= 0x12) return 8;
    if (type == 0x13) return 24;
    if (type == 0x14) return 112;
    return SIZE_MAX;
}

struct Command {
    uint32_t type;
    uint32_t id;
    uint64_t context;
    uint32_t arg;
    uint8_t payload[112];
};

struct AcpHal;
WINAPI HRESULT acp_connect(AcpHal* self, uint32_t cmd_entries, uint32_t msg_entries);
WINAPI HRESULT acp_disconnect(AcpHal* self);
WINAPI HRESULT acp_submit(AcpHal* self, uint32_t type, uint64_t context, uint32_t id, const void* data, uint32_t arg);
WINAPI bool acp_pop_message(AcpHal* self, AcpMessage* out);
WINAPI uint32_t acp_num_messages(AcpHal* self);
WINAPI void acp_release(AcpHal* self);
WINAPI HRESULT acp_notimpl(AcpHal*) { return 0x8AC80F0E; }
WINAPI AcpHal* acp_delete(AcpHal* self, unsigned flags);

void* g_acp_vt[] = {(void*)&acp_connect,      (void*)&acp_disconnect, (void*)&acp_submit, (void*)&acp_pop_message,
                    (void*)&acp_num_messages, (void*)&acp_release,    (void*)&acp_notimpl, (void*)&acp_notimpl,
                    (void*)&acp_notimpl,      (void*)&acp_notimpl,    (void*)&acp_delete};

struct AcpHal {
    void** vt = g_acp_vt;
    std::mutex m;
    std::condition_variable cv;
    bool connected = false;
    uint32_t cmd_cap = 0, msg_cap = 0;
    uint32_t msg_mask = 0;
    std::deque<Command> commands;
    std::deque<AcpMessage> messages;
    std::thread worker;
    bool stop = false;
    uint64_t frames = 0;

    void post(const AcpMessage& msg) {
        if (!(msg_mask & msg.type)) return;
        if (messages.size() >= msg_cap) {
            FL_ERROR("ACP: message queue full, dropping type 0x%x", msg.type);
            return;
        }
        messages.push_back(msg);
    }

    // One audio frame: execute what is pending, in submission order. A
    // flowgraph takes one frame. `id` is a scheduled frame (FMOD passes
    // 0xffffffff = now) and `arg` a notification mode (FMOD passes 0).
    void tick() {
        std::lock_guard lock(m);
        frames++;
        auto post_fn = [this](const shape::Message& msg) {
            AcpMessage a;
            memcpy(&a, &msg, sizeof a);
            post(a);
        };
        while (!commands.empty()) {
            Command c = commands.front();
            commands.pop_front();
            static std::atomic<bool> warned{false};
            if ((c.id != 0xffffffff || c.arg) && !warned.exchange(true))
                FL_ERROR("ACP: scheduled/notifying commands are not implemented, run now (type %u id 0x%x arg 0x%x)",
                         c.type, c.id, c.arg);
            bool graph = false;
            if (c.type == kCmdMessageMask || c.type == kCmdMessageUnmask) {
                uint32_t mask;
                memcpy(&mask, c.payload, 4);
                msg_mask = c.type == kCmdMessageMask ? msg_mask | mask : msg_mask & ~mask;
                FL_LOG(audio, "ACP: message mask 0x%x", msg_mask);
            } else if (c.type == kCmdFlowgraph) {
                uint32_t count, apu;
                memcpy(&count, c.payload, 4);
                memcpy(&apu, c.payload + 4, 4);
                shape::run_graph(count, apu, post_fn);
                graph = true;
            } else if (!shape::command(c.type, c.payload)) {
                static std::set<uint32_t> seen;
                if (seen.insert(c.type).second)
                    FL_ERROR("UNIMPLEMENTED ACP command type %u (id %u, context 0x%llx) - ignored", c.type, c.id,
                             (unsigned long long)c.context);
            }
            if (c.context)
                post({kMsgCommandDone, {0, c.type, uint32_t(c.context), uint32_t(c.context >> 32), shape::frame()}});
            if (graph) break;  // one flowgraph per audio frame
        }
    }

    void run() {
        using namespace std::chrono;
        auto period = duration_cast<steady_clock::duration>(duration<double>(128.0 / 48000.0));
        auto next = steady_clock::now();
        std::unique_lock lock(m);
        while (!stop) {
            next += period;
            cv.wait_until(lock, next, [&] { return stop; });
            if (stop) break;
            lock.unlock();
            tick();
            lock.lock();
        }
    }
};

WINAPI HRESULT acp_connect(AcpHal* self, uint32_t cmd_entries, uint32_t msg_entries) {
    if (!cmd_entries) return E_INVALIDARG_;
    std::lock_guard lock(self->m);
    if (self->connected) return 0x8AC80009;
    self->cmd_cap = cmd_entries;
    self->msg_cap = msg_entries;
    self->connected = true;
    self->stop = false;
    self->worker = std::thread([self] { self->run(); });
    FL_LOG(audio, "ACP: connect (%u command / %u message entries)", cmd_entries, msg_entries);
    return S_OK;
}

WINAPI HRESULT acp_disconnect(AcpHal* self) {
    {
        std::lock_guard lock(self->m);
        if (!self->connected) return 0x8AC8000A;
        self->stop = true;
        self->connected = false;
    }
    self->cv.notify_all();
    if (self->worker.joinable()) self->worker.join();
    std::lock_guard lock(self->m);
    self->commands.clear();
    self->messages.clear();
    return S_OK;
}

WINAPI HRESULT acp_submit(AcpHal* self, uint32_t type, uint64_t context, uint32_t id, const void* data, uint32_t arg) {
    std::lock_guard lock(self->m);
    if (!self->connected) return 0x8000000E;  // E_ILLEGAL_METHOD_CALL
    size_t n = payload_size(type);
    if (n == SIZE_MAX || (n && !data)) return E_INVALIDARG_;
    if (self->commands.size() >= self->cmd_cap) return 0x8AC80003;  // queue full
    Command c{type, id, context, arg, {}};
    if (n) memcpy(c.payload, data, n);
    static std::atomic<int> logged{0};
    if (logged++ < 40) {
        uint32_t w0 = 0;
        if (n >= 4) memcpy(&w0, c.payload, 4);
        FL_LOG(audio, "ACP submit type %u context 0x%llx id 0x%x arg 0x%x payload[0] 0x%x", type,
               (unsigned long long)context, id, arg, w0);
    }
    self->commands.push_back(c);
    return S_OK;
}

WINAPI bool acp_pop_message(AcpHal* self, AcpMessage* out) {
    std::lock_guard lock(self->m);
    if (self->messages.empty() || !out) return false;
    *out = self->messages.front();
    self->messages.pop_front();
    return true;
}

WINAPI uint32_t acp_num_messages(AcpHal* self) {
    std::lock_guard lock(self->m);
    return uint32_t(self->messages.size());
}

WINAPI AcpHal* acp_delete(AcpHal* self, unsigned flags) {
    if (self->connected) acp_disconnect(self);
    if (flags & 1) delete self;
    return self;
}

WINAPI void acp_release(AcpHal* self) {
    if (self) acp_delete(self, 1);
}

WINAPI HRESULT AcpHalCreate_(AcpHal** out) {
    if (!out) return E_INVALIDARG_;
    *out = new AcpHal();
    return S_OK;
}

}  // namespace

const ShapeArrays& shape_arrays() {
    static ShapeArrays a;
    a = {static_cast<uint8_t*>(g_contexts.src), static_cast<uint8_t*>(g_contexts.dma), static_cast<uint8_t*>(g_contexts.xma),
         g_contexts.num_src, g_contexts.num_dma, g_contexts.num_xma};
    return a;
}

uint8_t* apu_va(uint32_t apu, uint32_t len) {
    std::lock_guard lock(g_heap_lock);
    for (auto& h : g_heaps)
        if (h.contains_apu(apu) && uint64_t(apu - h.apu_base) + len <= h.size) return h.base + (apu - h.apu_base);
    return nullptr;
}

HLE_EXPORT(acphal, ApuCreateHeap, ApuCreateHeap_);
HLE_EXPORT(acphal, ApuAlloc, ApuAlloc_);
HLE_EXPORT(acphal, ApuFree, ApuFree_);
HLE_EXPORT(acphal, ApuMapVirtualAddress, ApuMapVirtualAddress_);
HLE_EXPORT(acphal, ApuMapApuAddress, ApuMapApuAddress_);
HLE_EXPORT(acphal, ApuIsVirtualAddressValid, ApuIsVirtualAddressValid_);
HLE_EXPORT(acphal, AcpHalAllocateShapeContexts, AcpHalAllocateShapeContexts_);
HLE_EXPORT(acphal, AcpHalReleaseShapeContexts, AcpHalReleaseShapeContexts_);
HLE_EXPORT(acphal, AcpHalCreate, AcpHalCreate_);

}  // namespace fl::acphal
