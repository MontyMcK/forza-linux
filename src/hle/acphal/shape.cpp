// SHAPE flowgraph execution (see shape.h). Clean-room from the observed
// protocol: FMOD's use of the contexts, and Azimuth's (CLAUDE.md Rule 1)
// checks and messages as evidence of the semantics (FINDINGS §4).
//
// Per audio frame (128 samples at 48 kHz) a flowgraph runs these opcodes, each
// a little-endian u64 (low 5 bits = opcode, bits 5-7 must be 0):
//   1    mix buffer allocate: buffer (u >> 33) & 0x1fff, 128 floats, zeroed.
//   2    SRC: context n = (u >> 13) & 0xfff (SRC and XMA context n go together)
//        resamples XMA voice n into mix buffer (u >> 25) & 0x1fff and, for
//        stereo voices, (u >> 38) & 0x1fff.
//   0x10 DMA: mix buffer (u >> 14) & 0x1fff into DMA context (u >> 27) & 0xfff;
//        bit 13 (the other direction) is not used by the game.
//
// XMA context (64 bytes) = the Xbox 360 XMA context layout:
//   w0  bits 0-11 buffer 0 packets, 12-19 loop count, 20/21 buffer 0/1 valid
//   w1  bits 0-11 buffer 1 packets, 24-26 output padding, 27-28 sample rate
//       (24/32/44.1/48 kHz), 29 stereo, 31 output valid
//   w2  bits 0-25 read offset in bits (packet << 14 | bit, 0x20 = first frame)
//   w4  bit 31 current buffer;  w5/w6 buffer 0/1 APU address (2048-byte packets)
// SRC context (32 bytes): w0 bits 0-20 frame stamp; w1 bits 0-20 rate (0x40000
//   = 1.0), bits 22-23 state (1 = playing an XMA voice); w2 input samples
//   consumed; w3 bits 0-17 phase; w4 bits 0-20 target rate.
// DMA context (16 bytes): w0 bits 0-20 frame stamp; w1 bits 0-4 read block,
//   8-12 write block, 16 full; w2 ring APU address; w3 bits 0-4 blocks - 1,
//   bits 5+ format (0x100 = one channel of 128 floats per block).
#include "hle/acphal/shape.h"

#include <cmath>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#include "common/log.h"

#if FL_HAVE_FFMPEG_XMA
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/mem.h>
}
#endif

namespace fl::acphal::shape {
namespace {

constexpr int kFrameSamples = 128;
constexpr uint32_t kPacketBytes = 0x800;

std::mutex g_mutex;
uint32_t g_frame = 0;

// Each odd case is logged once (by message), with the first value seen.
void log_once(const char* what, uint64_t value) {
    static std::mutex m;
    static std::set<const char*> seen;
    std::lock_guard lock(m);
    if (seen.insert(what).second) FL_ERROR("%s (0x%llx)", what, (unsigned long long)value);
}

// Decodes one stream of 2048-byte XMA2 packets to interleaved floats.
struct Decoder {
    virtual ~Decoder() = default;
    virtual void packet(const uint8_t* p, std::vector<float>& out) = 0;
};

// STUB: without the FFmpeg XMA decoder (tools/build_ffmpeg_xma.sh) each packet
// is consumed at its real rate (header byte 0 >> 2 = XMA frames starting in
// it, 512 samples per channel each) and plays as silence.
struct SilentDecoder : Decoder {
    int channels;
    explicit SilentDecoder(int ch) : channels(ch) {}
    void packet(const uint8_t* p, std::vector<float>& out) override {
        out.insert(out.end(), size_t(p[0] >> 2) * 512 * channels, 0.0f);
    }
};

#if FL_HAVE_FFMPEG_XMA
struct FfmpegDecoder : Decoder {
    AVCodecContext* c = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frm = nullptr;
    int channels;
    FfmpegDecoder(int ch, int rate) : channels(ch) {
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
        if (!codec || !(c = avcodec_alloc_context3(codec))) return;
        c->sample_rate = rate;
        av_channel_layout_default(&c->ch_layout, ch);
        c->block_align = kPacketBytes;
        // XMA2WAVEFORMATEX as FFmpeg reads it (34 bytes): u16 streams, u32 channel mask.
        c->extradata = static_cast<uint8_t*>(av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE));
        c->extradata_size = 34;
        AV_WL16(c->extradata, 1);
        AV_WL32(c->extradata + 2, ch == 2 ? 3 : 4);
        if (avcodec_open2(c, codec, nullptr) < 0) {
            FL_ERROR("SHAPE: FFmpeg XMA2 decoder (%d ch, %d Hz) failed to open", ch, rate);
            avcodec_free_context(&c);
            return;
        }
        pkt = av_packet_alloc();
        frm = av_frame_alloc();
    }
    ~FfmpegDecoder() override {
        av_frame_free(&frm);
        av_packet_free(&pkt);
        avcodec_free_context(&c);
    }
    bool ok() const { return c && pkt && frm; }
    void packet(const uint8_t* p, std::vector<float>& out) override {
        if (av_new_packet(pkt, kPacketBytes) < 0) return;
        memcpy(pkt->data, p, kPacketBytes);
        int r = avcodec_send_packet(c, pkt);
        av_packet_unref(pkt);
        if (r < 0) {
            log_once("SHAPE: FFmpeg rejected an XMA packet", static_cast<uint32_t>(-r));
            return;
        }
        while (avcodec_receive_frame(c, frm) == 0) {
            int n = frm->nb_samples, ch = std::min(channels, frm->ch_layout.nb_channels);
            size_t base = out.size();
            out.resize(base + size_t(n) * channels, 0.0f);
            for (int k = 0; k < ch; k++) {
                const float* s = reinterpret_cast<const float*>(frm->extended_data[k]);  // FLTP
                for (int i = 0; i < n; i++) out[base + size_t(i) * channels + k] = s[i];
            }
            av_frame_unref(frm);
        }
    }
};
#endif

std::unique_ptr<Decoder> make_decoder(int channels, int rate) {
#if FL_HAVE_FFMPEG_XMA
    auto d = std::make_unique<FfmpegDecoder>(channels, rate);
    if (d->ok()) return d;
#endif
    FL_STUB_ONCE("SHAPE: no FFmpeg XMA decoder (tools/build_ffmpeg_xma.sh): XMA voices play as silence");
    (void)rate;
    return std::make_unique<SilentDecoder>(channels);
}

// FL_DUMP_XMA=<dir>: the first 40 voices' input packets (v<n>.xma, 2048-byte
// packets as fed to the decoder) and decoded output (v<n>.f32, interleaved
// floats), plus v<n>.txt (channels, rate), to study decoding offline.
struct Dump {
    FILE* xma = nullptr;
    FILE* pcm = nullptr;
};
Dump open_dump(int channels, int rate) {
    static const char* dir = getenv("FL_DUMP_XMA");
    static int count = 0;
    Dump d;
    if (!dir || count >= 40) return d;
    int n = count++;
    std::string base = std::string(dir) + "/v" + std::to_string(n);
    d.xma = fopen((base + ".xma").c_str(), "wb");
    d.pcm = fopen((base + ".f32").c_str(), "wb");
    if (FILE* t = fopen((base + ".txt").c_str(), "w")) {
        fprintf(t, "%d %d\n", channels, rate);
        fclose(t);
    }
    return d;
}

struct Voice {
    std::unique_ptr<Decoder> dec;
    Dump dump;
    int channels = 1;
    int rate = 48000;
    std::vector<float> pcm;  // decoded, interleaved
    size_t pos = 0;          // read index into pcm (samples x channels)
    size_t decoded = 0;      // sample frames decoded so far (FL_LOG=audio)
    uint32_t steps = 0, inactive = 0, starved_steps = 0, zero_rate = 0;  // src_step outcomes (FL_LOG=audio)
    uint32_t start_frame = 0;
    bool starved = false;
};
std::map<uint32_t, Voice> g_voices;  // by XMA context index
uint32_t g_starved_frames = 0;       // FL_LOG=audio statistics
uint32_t g_dma_overwrites = 0, g_dma_overreads = 0, g_dma_writes = 0;
uint32_t g_ring_blocks = 0;

volatile uint32_t* ctx(uint8_t* array, uint32_t count, uint32_t size, uint32_t i) {
    return array && i < count ? reinterpret_cast<volatile uint32_t*>(array + size_t(i) * size) : nullptr;
}
volatile uint32_t* xma_ctx(uint32_t i) { return ctx(shape_arrays().xma, shape_arrays().num_xma, 64, i); }
volatile uint32_t* src_ctx(uint32_t i) { return ctx(shape_arrays().src, shape_arrays().num_src, 32, i); }
volatile uint32_t* dma_ctx(uint32_t i) { return ctx(shape_arrays().dma, shape_arrays().num_dma, 16, i); }

void stamp(volatile uint32_t* w) { w[0] = (w[0] & ~0x1fffffu) | (g_frame & 0x1fffff); }

// Decode packets of XMA context x until `need` sample frames are buffered.
// Packets are consumed whole: the read offset stays at packet << 14 | 0x20; the
// last packet of a buffer clears its valid bit and switches buffers.
bool fill(Voice& v, volatile uint32_t* x, size_t need) {
    while ((v.pcm.size() - v.pos) / v.channels < need) {
        uint32_t cur = x[4] >> 31;
        if (!(x[0] >> (20 + cur) & 1)) {
            uint32_t other = cur ^ 1;
            if (!(x[0] >> (20 + other) & 1)) return false;  // nothing queued: starved
            x[2] = (x[2] & 0xfc000000) | 0x20;
            x[4] = (x[4] & 0x7fffffff) | other << 31;
            cur = other;
        }
        uint32_t packets = x[cur] & 0xfff;
        uint32_t pkt = (x[2] & 0x3ffffff) >> 14;
        if (pkt >= packets) {
            log_once("SHAPE: XMA read position past its buffer; buffer dropped", pkt);
            x[0] = x[0] & ~(1u << (20 + cur));
            continue;
        }
        if ((x[2] & 0x3fff) != 0x20) log_once("SHAPE: XMA read starts inside a packet (not seeked)", x[2] & 0x3fff);
        const uint8_t* p = apu_va(x[5 + cur] + pkt * kPacketBytes, kPacketBytes);
        if (!p) {
            log_once("SHAPE: XMA packet outside the APU heaps; buffer dropped", x[5 + cur]);
            x[0] = x[0] & ~(1u << (20 + cur));
            continue;
        }
        if (v.pos > (1u << 16)) {  // keep the buffer small
            v.pcm.erase(v.pcm.begin(), v.pcm.begin() + v.pos);
            v.pos = 0;
        }
        size_t before = v.pcm.size();
        v.dec->packet(p, v.pcm);
        v.decoded += (v.pcm.size() - before) / v.channels;
        if (v.dump.xma) {
            fwrite(p, 1, kPacketBytes, v.dump.xma);
            fwrite(v.pcm.data() + before, sizeof(float), v.pcm.size() - before, v.dump.pcm);
            fflush(v.dump.xma);
            fflush(v.dump.pcm);
        }
        if (++pkt == packets) {
            x[4] = x[4] ^ 0x80000000;
            x[0] = x[0] & ~(1u << (20 + cur));
            pkt = 0;
        }
        x[2] = (pkt * 0x4000 + 0x20) | (x[2] & 0xfc000000);
    }
    return true;
}

// Opcode 2 for one frame.
void src_step(uint32_t n, float* left, float* right, uint32_t graph_apu, const Post& post) {
    volatile uint32_t* s = src_ctx(n);
    volatile uint32_t* x = xma_ctx(n);
    if (!s || !x) {
        log_once("SHAPE: SRC context outside the arrays", n);
        return;
    }
    auto it = g_voices.find(n);
    if ((s[1] & 0xc00000) != 0x400000 || !(x[1] >> 31)) {
        log_once("SHAPE: SRC step on a voice that is not an active XMA output", s[1]);
        if (it != g_voices.end()) it->second.inactive++;
        return;
    }
    uint32_t rate = s[1] & 0x1fffff, target = s[4] & 0x1fffff;
    if (rate != target) log_once("SHAPE: SRC rate ramps are not implemented (using the target rate)", target);
    rate = target;
    if (!rate) {
        if (it != g_voices.end()) it->second.zero_rate++;
        return;
    }
    if (it == g_voices.end()) {
        static const int kRates[4] = {24000, 32000, 44100, 48000};
        Voice v;
        v.channels = int(x[1] >> 29 & 1) + 1;
        v.rate = kRates[x[1] >> 27 & 3];
        v.dec = make_decoder(v.channels, v.rate);
        v.dump = open_dump(v.channels, v.rate);
        if (s[2] || (s[3] & 0x1ffffff)) log_once("SHAPE: XMA voice starts from an already rendered SRC context", n);
        v.start_frame = g_frame;
        it = g_voices.emplace(n, std::move(v)).first;
        FL_LOG(audio, "SHAPE: XMA voice %u: %d ch %d Hz rate 0x%x; loops %u start %u end %u subframes %u/%u/%u "
               "decode %u; packets %u/%u", n, it->second.channels, it->second.rate, rate, x[0] >> 12 & 0xff,
               x[3] & 0x3ffffff, x[4] & 0x3ffffff, x[1] >> 12 & 3, x[1] >> 14 & 7, x[1] >> 17 & 7, x[1] >> 20 & 15,
               x[0] & 0xfff, x[1] & 0xfff);
    }
    Voice& v = it->second;
    v.steps++;
    uint32_t frac = s[3] & 0x3ffff;
    uint64_t adv = uint64_t(rate) * kFrameSamples + frac;
    uint32_t consumed = uint32_t(adv >> 18);
    if (!fill(v, x, consumed + 2)) {  // +2: linear interpolation reads one sample ahead
        g_starved_frames++;
        v.starved_steps++;
        if (!v.starved) post({4, {0, n, graph_apu, 0, 0}});
        v.starved = true;
        return;
    }
    v.starved = false;
    const float* in = v.pcm.data() + v.pos;
    int ch = v.channels;
    for (int i = 0; i < kFrameSamples; i++) {
        uint64_t p = uint64_t(rate) * i + frac;
        size_t k = size_t(p >> 18);
        float t = float(p & 0x3ffff) * (1.0f / 262144.0f);
        if (left) left[i] = in[k * ch] * (1 - t) + in[(k + 1) * ch] * t;
        if (right && ch == 2) right[i] = in[k * ch + 1] * (1 - t) + in[(k + 1) * ch + 1] * t;
    }
    v.pos += size_t(consumed) * ch;
    stamp(s);
    s[2] = s[2] + consumed;
    s[3] = uint32_t(adv & 0x3ffff) | (s[3] & 0xfe000000);
}

// Opcode 0x10 for one frame.
void dma_step(const float* mix, uint32_t n) {
    volatile uint32_t* d = dma_ctx(n);
    if (!d) {
        log_once("SHAPE: DMA context outside the array", n);
        return;
    }
    if ((d[3] & ~0x1fu) != 0x100) log_once("SHAPE: unknown DMA format", d[3] & ~0x1fu);
    uint32_t blocks = (d[3] & 0x1f) + 1;
    auto* ring = reinterpret_cast<float*>(apu_va(d[2], blocks * kFrameSamples * 4));
    if (!ring) {
        log_once("SHAPE: DMA ring outside the APU heaps", d[2]);
        return;
    }
    uint32_t w = d[1] >> 8 & 0x1f, r = d[1] & 0x1f;
    g_dma_writes++;
    g_ring_blocks = blocks;
    if (d[1] >> 16 & 1) g_dma_overwrites++;  // full: this block was not read yet
    if (mix) memcpy(ring + w * kFrameSamples, mix, kFrameSamples * 4);
    else memset(ring + w * kFrameSamples, 0, kFrameSamples * 4);
    w = (w + 1) % blocks;
    d[1] = (w == r ? 0x10000u : 0u) | w << 8 | (d[1] & 0xfffee0ff);
    stamp(d);
}

}  // namespace

bool command(uint32_t type, const uint8_t* payload) {
    std::lock_guard lock(g_mutex);
    uint32_t p[6] = {};
    switch (type) {
        case 5: case 6: case 7: case 8: {  // XMA start / stop: one index or a 512-bit mask
            uint32_t mask[16] = {};
            if (type == 5 || type == 7) {
                memcpy(p, payload, 4);
                if (p[0] < 512) mask[p[0] >> 5] = 1u << (p[0] & 31);
            } else {
                memcpy(mask, payload, 64);
            }
            for (uint32_t i = 0; i < 512; i++)
                if (mask[i >> 5] >> (i & 31) & 1) {
                    if (type >= 7) {
                        auto it = g_voices.find(i);
                        if (it != g_voices.end() && log_enabled("audio")) {
                            const Voice& v = it->second;
                            volatile uint32_t* x = xma_ctx(i);
                            volatile uint32_t* sc = src_ctx(i);
                            FL_LOG(audio, "SHAPE: XMA %u stopped: decoded %zu, played %u, left unplayed %zu samples; "
                                   "input valid bits %u; SRC steps %u (inactive %u, starved %u, rate 0 %u) over %u "
                                   "graphs", i, v.decoded, sc ? sc[2] : 0, (v.pcm.size() - v.pos) / v.channels,
                                   x ? (x[0] >> 20 & 3) : 0, v.steps, v.inactive, v.starved_steps, v.zero_rate,
                                   g_frame - v.start_frame);
                        }
                        g_voices.erase(i);  // stop: the next SRC step starts a new voice
                    }
                    FL_LOG(audio, "SHAPE: XMA %u %s", i, type >= 7 ? "stop" : "start");
                }
            return true;
        }
        case 0x10: {  // DMA consumption {u32 dma, u32 blocks read by the CPU}
            memcpy(p, payload, 8);
            volatile uint32_t* d = dma_ctx(p[0]);
            if (!d) return true;
            uint32_t blocks = (d[3] & 0x1f) + 1;
            uint32_t w = d[1] >> 8 & 0x1f, r = d[1] & 0x1f;
            uint32_t ready = d[1] >> 16 & 1 ? blocks : (w + blocks - r) % blocks;
            if (p[1] > ready) g_dma_overreads += p[1] - ready;  // FMOD read blocks we had not written
            d[1] = ((d[1] & 0x1f) + p[1]) % blocks | (d[1] & 0xfffeffe0);
            return true;
        }
        case 0x13: {  // XMA input update {xma, buffer, packets, set packets, set pointer, pointer}
            memcpy(p, payload, 24);
            volatile uint32_t* x = xma_ctx(p[0]);
            if (!x || p[1] > 1) {
                log_once("SHAPE: XMA input update for a bad context/buffer", p[0]);
                return true;
            }
            if (x[0] >> (20 + p[1]) & 1) log_once("SHAPE: XMA input replaced before it was consumed", p[0]);
            if (p[3]) x[p[1]] = (x[p[1]] & 0xfffff000) | (p[2] & 0xfff);
            if (p[4]) x[5 + p[1]] = p[5];
            x[0] = x[0] | 1u << (20 + p[1]);
            return true;
        }
    }
    return false;
}

void run_graph(const uint64_t* ops, uint32_t count, uint32_t apu, const Post& post) {
    std::lock_guard lock(g_mutex);
    // FL_LOG=audio: every 5 s (1875 frames), voice-frames that had no decoded
    // input (starved) and how late the graphs run against real time.
    static int64_t t0 = 0;
    static uint32_t graphs = 0;
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = ts.tv_sec * 1000000000LL + ts.tv_nsec;
    if (!t0) t0 = now;
    if (++graphs == 1875) {
        FL_LOG(audio, "SHAPE: %u graphs in %.2f s (real time: 5.00 s), %u starved voice-frames; DMA %u block "
               "writes (ring %u blocks), %u overwrote unread blocks, %u blocks read before written", graphs,
               (now - t0) / 1e9, g_starved_frames, g_dma_writes, g_ring_blocks, g_dma_overwrites, g_dma_overreads);
        graphs = 0;
        g_starved_frames = g_dma_writes = g_dma_overwrites = g_dma_overreads = 0;
        t0 = now;
    }
    if (!ops) {
        log_once("SHAPE: flowgraph outside the APU heaps", apu);
        post({2, {0, apu, 0, 0, 0}});
        return;
    }
    std::map<uint32_t, std::vector<float>> mix;  // mix buffers of this frame
    auto buffer = [&](uint32_t i) -> float* {
        auto it = mix.find(i);
        return it == mix.end() ? nullptr : it->second.data();
    };
    for (uint32_t i = 0; i < count; i++) {
        uint64_t u = ops[i];
        if (u & 0xe0) log_once("SHAPE: flowgraph opcode with queued/disabled/error flags", uint32_t(u));
        switch (u & 0x1f) {
            case 1: mix[uint32_t(u >> 33) & 0x1fff].assign(kFrameSamples, 0.0f); break;
            case 2:
                src_step(uint32_t(u >> 13) & 0xfff, buffer(uint32_t(u >> 25) & 0x1fff), buffer(uint32_t(u >> 38) & 0x1fff),
                         apu, post);
                break;
            case 0x10:
                if (u >> 13 & 1) log_once("SHAPE: DMA opcode in the other direction", uint32_t(u));
                dma_step(buffer(uint32_t(u >> 14) & 0x1fff), uint32_t(u >> 27) & 0xfff);
                break;
            default: log_once("UNIMPLEMENTED SHAPE flowgraph opcode", uint32_t(u & 0x1f));
        }
    }
    post({2, {0, apu, 0, 0, 0}});
    g_frame++;
}

uint32_t frame() { return g_frame; }

void reset() {
    std::lock_guard lock(g_mutex);
    g_voices.clear();
    g_frame = 0;
}

}  // namespace fl::acphal::shape
