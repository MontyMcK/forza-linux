// WASAPI (MMDevAPI): IMMDeviceEnumerator -> IMMDevice -> IAudioClient /
// IAudioRenderClient / IAudioClock. This is FMOD's output path.
//
// Interfaces are the public Windows ones (mmdeviceapi.h, audioclient.h).
// Two ways in: CoCreateInstance(MMDeviceEnumerator) and, for FMOD,
// ActivateAudioInterfaceAsync (IAudioClient2).
// Backend: an SDL3 audio stream (PipeWire/Pulse). Padding is what SDL still
// has queued; a device thread signals the client's event every period. With
// FL_AUDIO=null (or no audio device) frames are consumed in real time and dropped.

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#include "common/log.h"
#include "hle/kernelx/sync_objects.h"
#include "hle/registry.h"
#include "hle/winrt/object.h"

namespace fl::audio {
namespace {

using winrt::Iface;
using winrt::Object;
using winrt::Vtable;
constexpr int U = winrt::kUnknownSlots;

constexpr GUID IID_IMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
constexpr GUID CLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
constexpr GUID IID_IMMDevice = {0xD666063F, 0x1587, 0x4E43, {0x81, 0xF1, 0xB9, 0x48, 0xE8, 0x07, 0x36, 0x3F}};
constexpr GUID IID_IAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
constexpr GUID IID_IAudioClient2 = {0x726778CD, 0xF60A, 0x4EDA, {0x82, 0xDE, 0xE4, 0x76, 0x10, 0xCD, 0x78, 0xAA}};
constexpr GUID IID_IActivateAudioInterfaceAsyncOperation = {
    0x72A22D78, 0xCDE4, 0x431D, {0xB8, 0xCC, 0x84, 0x3A, 0x71, 0x19, 0x9B, 0x6D}};
constexpr GUID IID_IAudioRenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
constexpr GUID IID_IAudioClock = {0xCD63314F, 0x3FBA, 0x4A1B, {0x81, 0x2C, 0xEF, 0x96, 0x35, 0x87, 0x28, 0xE7}};
constexpr GUID KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

constexpr HRESULT AUDCLNT_E_NOT_INITIALIZED = static_cast<HRESULT>(0x88890001);
constexpr HRESULT AUDCLNT_E_BUFFER_TOO_LARGE = static_cast<HRESULT>(0x88890006);
constexpr DWORD AUDCLNT_STREAMFLAGS_EVENTCALLBACK = 0x00040000;

#pragma pack(push, 1)
struct WaveFormatEx {
    WORD wFormatTag, nChannels;
    DWORD nSamplesPerSec, nAvgBytesPerSec;
    WORD nBlockAlign, wBitsPerSample, cbSize;
};
struct WaveFormatExtensible {
    WaveFormatEx Format;
    WORD wValidBitsPerSample;
    DWORD dwChannelMask;
    GUID SubFormat;
};
#pragma pack(pop)
static_assert(sizeof(WaveFormatEx) == 18 && sizeof(WaveFormatExtensible) == 40);

// The device's mix format: 48 kHz float stereo (a typical Linux desktop).
// The Xbox would offer up to 7.1; the game asks via the Xbox channel-count
// interface below and adapts.
constexpr int kRate = 48000;
constexpr int kChannels = 2;
constexpr DWORD kChannelMask = 0x3;  // FL FR

WaveFormatExtensible mix_format() {
    WaveFormatExtensible f{};
    f.Format.wFormatTag = 0xFFFE;
    f.Format.nChannels = kChannels;
    f.Format.nSamplesPerSec = kRate;
    f.Format.wBitsPerSample = 32;
    f.Format.nBlockAlign = kChannels * 4;
    f.Format.nAvgBytesPerSec = kRate * kChannels * 4;
    f.Format.cbSize = 22;
    f.wValidBitsPerSample = 32;
    f.dwChannelMask = kChannelMask;
    f.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return f;
}

void* co_task_alloc(size_t n);  // from combase (CoTaskMemAlloc), see below

// ---------------------------------------------------------------- IAudioClient + render client + clock

struct AudioClient : Object {
    std::mutex m;
    bool initialized = false;
    bool running = false;
    uint32_t block = 0;          // bytes per frame
    uint32_t rate = kRate;
    uint32_t buffer_frames = 0;  // total buffer size
    uint32_t padding = 0;        // frames queued, not yet "played"
    uint64_t played = 0;         // frames consumed by the device
    HANDLE event = nullptr;
    std::vector<uint8_t> staging;
    std::atomic<bool> quit{false};
    std::thread device;
    SDL_AudioStream* stream = nullptr;  // null: drop frames (null backend)
    uint64_t pushed = 0;                // frames handed to SDL
    int device_frames = 0;              // frames the device pulls at a time
    uint32_t checks = 0, low = 0;       // FL_LOG=audio statistics

    ~AudioClient() override {
        quit = true;
        if (device.joinable()) device.join();
        if (stream) SDL_DestroyAudioStream(stream);
    }

    // Frames SDL has not played yet (the WASAPI "padding"). Caller holds m.
    void update_padding() {
        if (!stream) return;
        uint32_t queued = static_cast<uint32_t>(std::max(0, SDL_GetAudioStreamQueued(stream)) / block);
        padding = std::min(queued, buffer_frames);
        played = pushed - padding;
    }

    void open_backend(const WaveFormatEx* fmt) {
        const char* env = getenv("FL_AUDIO");
        if (env && !strcmp(env, "null")) return;
        SDL_AudioFormat sf = SDL_AUDIO_F32;
        bool is_float = fmt->wFormatTag == 3;
        if (fmt->wFormatTag == 0xFFFE)
            is_float = reinterpret_cast<const WaveFormatExtensible*>(fmt)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        if (!is_float) sf = fmt->wBitsPerSample == 16 ? SDL_AUDIO_S16 : SDL_AUDIO_S32;
        if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            FL_ERROR("audio: SDL_InitSubSystem(AUDIO) failed: %s (sound disabled)", SDL_GetError());
            return;
        }
        SDL_AudioSpec spec{sf, fmt->nChannels, static_cast<int>(fmt->nSamplesPerSec)};
        // Ask for ~10 ms device periods (PipeWire's default quantum here is
        // 1024 frames, 21 ms); FL_AUDIO_FRAMES overrides.
        const char* frames_env = getenv("FL_AUDIO_FRAMES");
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames_env ? frames_env : "480");
        stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!stream) {
            FL_ERROR("audio: SDL_OpenAudioDeviceStream failed: %s (sound disabled)", SDL_GetError());
            return;
        }
        SDL_AudioSpec dev{};
        if (!SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream), &dev, &device_frames)) device_frames = 0;
        fl::log_write("audio", "SDL audio stream open (%s), device period %d frames", SDL_GetCurrentAudioDriver(),
                      device_frames);
    }

    void device_loop() {
        using namespace std::chrono;
        constexpr int kPeriodMs = 5;  // refill events: FMOD tops the buffer up between device pulls
        auto next = steady_clock::now();
        while (!quit) {
            next += milliseconds(kPeriodMs);
            std::this_thread::sleep_until(next);
            HANDLE ev = nullptr;
            {
                std::lock_guard lock(m);
                if (!running) continue;
                if (stream) {
                    update_padding();
                    // FL_LOG=audio: how often the queue nearly ran dry (less
                    // than one device period left: the next pull underruns).
                    ++checks;
                    if (padding < static_cast<uint32_t>(device_frames)) ++low;
                    if (checks == 500) {
                        FL_LOG(audio, "WASAPI: %u of %u checks (5 ms apart) had under one device period queued "
                               "(%d frames); buffer %u", low, checks, device_frames, buffer_frames);
                        checks = low = 0;
                    }
                } else {
                    uint32_t consume = std::min<uint32_t>(padding, rate * kPeriodMs / 1000);
                    padding -= consume;
                    played += consume;
                }
                ev = event;
            }
            if (ev) kx::signal_event(ev);
        }
    }

    static AudioClient* self(Iface* i) { return i->as<AudioClient>(); }

    static WINAPI HRESULT Initialize(Iface* i, int share, DWORD flags, int64_t duration, int64_t period,
                                     const WaveFormatEx* fmt, const GUID* session) {
        AudioClient* c = self(i);
        std::lock_guard lock(c->m);
        c->block = fmt->nBlockAlign;
        c->rate = fmt->nSamplesPerSec;
        // duration is in 100 ns units; keep at least 20 ms.
        c->buffer_frames = static_cast<uint32_t>(std::max<int64_t>(duration, 200000) * c->rate / 10000000);
        c->initialized = true;
        c->open_backend(fmt);
        // The game can't queue more than the buffer. The device pulls
        // device_frames at a time on its own clock, and FMOD writes one 10 ms
        // block per event when there is room for it. FMOD's 20 ms (960 frames)
        // against PipeWire's 1024-frame quantum left a 64-frame gap every 1024
        // frames (the "bad cable" crackle); with 480-frame periods the two
        // clocks drifted so two pulls fell between writes ~12 times a second.
        // Four device periods (~40 ms) ride that out.
        c->buffer_frames = std::max<uint32_t>(c->buffer_frames, static_cast<uint32_t>(c->device_frames) * 4);
        c->staging.resize(static_cast<size_t>(c->buffer_frames) * c->block);
        fl::log_write("audio", "IAudioClient::Initialize: %u Hz, %u ch, %u bits, tag 0x%x, buffer %u frames, flags 0x%x%s"
                      " (asked duration %lld period %lld, share %d)",
                      fmt->nSamplesPerSec, fmt->nChannels, fmt->wBitsPerSample, fmt->wFormatTag, c->buffer_frames,
                      flags, (flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) ? " (event driven)" : "", (long long)duration,
                      (long long)period, share);
        if (!c->device.joinable()) c->device = std::thread([c] { c->device_loop(); });
        return S_OK;
    }
    static WINAPI HRESULT GetBufferSize(Iface* i, uint32_t* out) {
        if (!self(i)->initialized) return AUDCLNT_E_NOT_INITIALIZED;
        *out = self(i)->buffer_frames;
        FL_LOG(audio, "IAudioClient::GetBufferSize -> %u", *out);
        return S_OK;
    }
    static WINAPI HRESULT GetStreamLatency(Iface*, int64_t* out) {
        *out = 100000;  // 10 ms
        return S_OK;
    }
    static WINAPI HRESULT GetCurrentPadding(Iface* i, uint32_t* out) {
        std::lock_guard lock(self(i)->m);
        self(i)->update_padding();
        *out = self(i)->padding;
        return S_OK;
    }
    static WINAPI HRESULT IsFormatSupported(Iface*, int share, const WaveFormatEx* fmt, WaveFormatEx** closest) {
        if (closest) *closest = nullptr;
        bool ok = fmt->nSamplesPerSec == kRate && fmt->nChannels == kChannels;
        FL_LOG(audio, "IsFormatSupported(%u Hz, %u ch, %u bits) -> %s", fmt->nSamplesPerSec, fmt->nChannels,
               fmt->wBitsPerSample, ok ? "yes" : "no");
        if (ok) return S_OK;
        if (closest) {
            auto* f = static_cast<WaveFormatExtensible*>(co_task_alloc(sizeof(WaveFormatExtensible)));
            *f = mix_format();
            *closest = &f->Format;
        }
        return S_FALSE;
    }
    static WINAPI HRESULT GetMixFormat(Iface*, WaveFormatEx** out) {
        FL_LOG(audio, "IAudioClient::GetMixFormat");
        auto* f = static_cast<WaveFormatExtensible*>(co_task_alloc(sizeof(WaveFormatExtensible)));
        *f = mix_format();
        *out = &f->Format;
        return S_OK;
    }
    static WINAPI HRESULT GetDevicePeriod(Iface*, int64_t* def, int64_t* min) {
        if (def) *def = 100000;  // 10 ms
        if (min) *min = 30000;   // 3 ms
        FL_LOG(audio, "IAudioClient::GetDevicePeriod");
        return S_OK;
    }
    static WINAPI HRESULT Start(Iface* i) {
        std::lock_guard lock(self(i)->m);
        self(i)->running = true;
        if (self(i)->stream) SDL_ResumeAudioStreamDevice(self(i)->stream);
        fl::log_write("audio", "IAudioClient::Start%s", self(i)->stream ? "" : " (null backend: audio is discarded)");
        return S_OK;
    }
    static WINAPI HRESULT Stop(Iface* i) {
        std::lock_guard lock(self(i)->m);
        self(i)->running = false;
        if (self(i)->stream) SDL_PauseAudioStreamDevice(self(i)->stream);
        return S_OK;
    }
    static WINAPI HRESULT Reset(Iface* i) {
        std::lock_guard lock(self(i)->m);
        if (self(i)->stream) SDL_ClearAudioStream(self(i)->stream);
        self(i)->padding = 0;
        return S_OK;
    }
    static WINAPI HRESULT SetEventHandle(Iface* i, HANDLE ev) {
        FL_LOG(audio, "IAudioClient::SetEventHandle(%p)", ev);
        self(i)->event = ev;
        return S_OK;
    }
    static WINAPI HRESULT GetService(Iface* i, const GUID& iid, void** out) {
        HRESULT hr = self(i)->query(iid, out);
        FL_LOG(audio, "IAudioClient::GetService(%s) -> 0x%x", winrt::guid_str(iid).c_str(), static_cast<unsigned>(hr));
        if (hr < 0) FL_ERROR("IAudioClient::GetService(%s): not supported", winrt::guid_str(iid).c_str());
        return hr;
    }

    // IAudioRenderClient
    static WINAPI HRESULT GetBuffer(Iface* i, uint32_t frames, uint8_t** data) {
        AudioClient* c = self(i);
        static std::atomic<uint64_t> calls{0};
        if ((calls++ % 500) == 0) FL_LOG(audio, "IAudioRenderClient::GetBuffer(%u) call %llu", frames, (unsigned long long)calls.load());
        std::lock_guard lock(c->m);
        c->update_padding();
        if (frames + c->padding > c->buffer_frames) return AUDCLNT_E_BUFFER_TOO_LARGE;
        *data = c->staging.data();
        return S_OK;
    }
    static WINAPI HRESULT ReleaseBuffer(Iface* i, uint32_t frames, DWORD flags) {
        AudioClient* c = self(i);
        std::lock_guard lock(c->m);
        if (c->stream) {
            size_t bytes = static_cast<size_t>(frames) * c->block;
            if (flags & 2 /* AUDCLNT_BUFFERFLAGS_SILENT */) memset(c->staging.data(), 0, bytes);
            SDL_PutAudioStreamData(c->stream, c->staging.data(), static_cast<int>(bytes));
            c->pushed += frames;
            c->update_padding();
        } else {
            c->padding += frames;
        }
        return S_OK;
    }

    // IAudioClock
    static WINAPI HRESULT GetFrequency(Iface* i, uint64_t* f) {
        *f = self(i)->rate;
        return S_OK;
    }
    static WINAPI HRESULT GetPosition(Iface* i, uint64_t* pos, uint64_t* qpc) {
        std::lock_guard lock(self(i)->m);
        self(i)->update_padding();
        *pos = self(i)->played;
        if (qpc) {
            timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            *qpc = static_cast<uint64_t>(ts.tv_sec) * 10000000 + ts.tv_nsec / 100;
        }
        return S_OK;
    }
    static WINAPI HRESULT GetCharacteristics(Iface*, DWORD* out) {
        *out = 0;
        return S_OK;
    }

    // IAudioClient2
    static WINAPI HRESULT IsOffloadCapable(Iface*, int category, int* out) {
        *out = 0;
        return S_OK;
    }
    static WINAPI HRESULT SetClientProperties(Iface*, const void*) { return S_OK; }
    static WINAPI HRESULT GetBufferSizeLimits(Iface*, const WaveFormatEx*, int, int64_t* min, int64_t* max) {
        *min = 30000;
        *max = 2000000;
        return S_OK;
    }

    static Vtable& vt_client() {
        static Vtable v = Vtable("IAudioClient2", U + 15, false)
                              .set(U + 0, &Initialize)
                              .set(U + 1, &GetBufferSize)
                              .set(U + 2, &GetStreamLatency)
                              .set(U + 3, &GetCurrentPadding)
                              .set(U + 4, &IsFormatSupported)
                              .set(U + 5, &GetMixFormat)
                              .set(U + 6, &GetDevicePeriod)
                              .set(U + 7, &Start)
                              .set(U + 8, &Stop)
                              .set(U + 9, &Reset)
                              .set(U + 10, &SetEventHandle)
                              .set(U + 11, &GetService)
                              .set(U + 12, &IsOffloadCapable)
                              .set(U + 13, &SetClientProperties)
                              .set(U + 14, &GetBufferSizeLimits);
        return v;
    }
    static Vtable& vt_render() {
        static Vtable v = Vtable("IAudioRenderClient", U + 2, false).set(U + 0, &GetBuffer).set(U + 1, &ReleaseBuffer);
        return v;
    }
    static Vtable& vt_clock() {
        static Vtable v = Vtable("IAudioClock", U + 3, false)
                              .set(U + 0, &GetFrequency)
                              .set(U + 1, &GetPosition)
                              .set(U + 2, &GetCharacteristics);
        return v;
    }

    Iface iclient{vt_client(), this};
    Iface irender{vt_render(), this};
    Iface iclock{vt_clock(), this};
    AudioClient() : Object(u"AudioClient") {
        expose(IID_IAudioClient, &iclient);
        expose(IID_IAudioClient2, &iclient);
        expose(IID_IAudioRenderClient, &irender);
        expose(IID_IAudioClock, &iclock);
    }
};

// ---------------------------------------------------------------- IMMDevice / IMMDeviceEnumerator

struct Device : Object {
    static WINAPI HRESULT Activate(Iface*, const GUID& iid, DWORD ctx, void* params, void** out) {
        FL_LOG(audio, "IMMDevice::Activate(%s)", winrt::guid_str(iid).c_str());
        auto* c = new AudioClient();
        HRESULT hr = c->query(iid, out);
        c->release();
        if (hr < 0) FL_ERROR("IMMDevice::Activate: unsupported interface %s", winrt::guid_str(iid).c_str());
        return hr;
    }
    static WINAPI HRESULT GetId(Iface*, char16_t** out) {
        static const char16_t kId[] = u"{0.0.0.00000000}.{forza-linux-default}";
        auto* p = static_cast<char16_t*>(co_task_alloc(sizeof kId));
        memcpy(p, kId, sizeof kId);
        *out = p;
        return S_OK;
    }
    static WINAPI HRESULT GetState(Iface*, DWORD* out) {
        *out = 1;  // DEVICE_STATE_ACTIVE
        return S_OK;
    }
    static Vtable& vt() {
        static Vtable v = Vtable("IMMDevice", U + 4, false).set(U + 0, &Activate).set(U + 2, &GetId).set(U + 3, &GetState);
        return v;
    }
    Iface idev{vt(), this};
    Device() : Object(u"MMDevice") { expose(IID_IMMDevice, &idev); }
};

// Xbox-only interface {8B557ADC-555E-47A0-B223-43477E481DAD}, QI'd from the
// device enumerator by the game's `HardwareChannelCount` class (rva 0xff5d08):
//   slot 8: HRESULT (uint32* a, uint32* b)   channel counts (game defaults 2, 2)
//   slot 9: HRESULT (callback*)              change notifications
//   slot 10: HRESULT (callback*)             unregister (its destructor, rva 0xff7640)
constexpr GUID IID_XboxHardwareChannels = {0x8B557ADC, 0x555E, 0x47A0, {0xB2, 0x23, 0x43, 0x47, 0x7E, 0x48, 0x1D, 0xAD}};

WINAPI HRESULT hw_GetChannelCounts(Iface*, uint32_t* a, uint32_t* b) {
    *a = kChannels;
    *b = kChannels;
    return S_OK;
}
WINAPI HRESULT hw_RegisterCallback(Iface*, void* cb) { return S_OK; }

struct Enumerator : Object {
    static WINAPI HRESULT GetDefaultAudioEndpoint(Iface*, int flow, int role, void** out) {
        FL_LOG(audio, "GetDefaultAudioEndpoint(flow %d, role %d)", flow, role);
        if (flow != 0 /* eRender */) {
            *out = nullptr;
            return static_cast<HRESULT>(0x80070490);  // E_NOTFOUND: no capture devices (no Kinect/mic)
        }
        static Device* dev = new Device();
        *out = dev->default_iface();
        return S_OK;
    }
    static WINAPI HRESULT GetDevice(Iface* i, const char16_t* id, void** out) {
        return GetDefaultAudioEndpoint(i, 0, 0, out);
    }
    static WINAPI HRESULT RegisterNotify(Iface*, void* cb) { return S_OK; }
    static Vtable& vt() {
        static Vtable v = Vtable("IMMDeviceEnumerator", U + 5, false)
                              .set(U + 1, &GetDefaultAudioEndpoint)
                              .set(U + 2, &GetDevice)
                              .set(U + 3, &RegisterNotify)
                              .set(U + 4, &RegisterNotify);
        return v;
    }
    static Vtable& vt_hw() {
        static Vtable v = Vtable("IXboxHardwareChannels{8B557ADC}", 12, false)
                              .set(8, &hw_GetChannelCounts)
                              .set(9, &hw_RegisterCallback)
                              .set(10, &hw_RegisterCallback);
        return v;
    }
    Iface ienum{vt(), this};
    Iface ihw{vt_hw(), this};
    Enumerator() : Object(u"MMDeviceEnumerator") {
        expose(IID_IMMDeviceEnumerator, &ienum);
        expose(IID_XboxHardwareChannels, &ihw);
    }
};

void* co_task_alloc(size_t n) { return malloc(n); }  // CoTaskMemFree == free in our combase

// IActivateAudioInterfaceAsyncOperation: slot 3 GetActivateResult(HRESULT*, IUnknown**).
struct ActivateOperation : Object {
    HRESULT result = S_OK;
    void* iface = nullptr;  // owning
    ~ActivateOperation() override {
        if (iface) winrt::com_release(iface);
    }
    static WINAPI HRESULT GetActivateResult(Iface* i, HRESULT* hr, void** out) {
        auto* op = i->as<ActivateOperation>();
        *hr = op->result;
        *out = op->iface;
        if (op->iface) winrt::com_add_ref(op->iface);
        return S_OK;
    }
    static Vtable& vt() {
        static Vtable v = Vtable("IActivateAudioInterfaceAsyncOperation", U + 1, false).set(U + 0, &GetActivateResult);
        return v;
    }
    Iface iop{vt(), this};
    ActivateOperation() : Object(u"ActivateAudioInterfaceAsyncOperation") {
        expose(IID_IActivateAudioInterfaceAsyncOperation, &iop);
    }
};

// Activation completes immediately: the handler's ActivateCompleted (slot 3)
// is called on the caller's thread before we return (it must run on a game
// thread anyway; FMOD waits for it right after the call).
WINAPI HRESULT ActivateAudioInterfaceAsync_(const char16_t* path, const GUID& iid, const void* params, void* handler,
                                            void** op_out) {
    FL_LOG(audio, "ActivateAudioInterfaceAsync(%s)", winrt::guid_str(iid).c_str());
    auto* op = new ActivateOperation();
    auto* c = new AudioClient();
    op->result = c->query(iid, &op->iface);
    c->release();
    if (op->result < 0) FL_ERROR("ActivateAudioInterfaceAsync: unsupported interface %s", winrt::guid_str(iid).c_str());
    *op_out = op->default_iface();  // the caller's reference
    if (handler) winrt::call_slot<HRESULT>(handler, 3, op->default_iface_noref());
    op->release();  // ours
    return S_OK;
}

// Like SetThreadAffinityMask it returns the PREVIOUS mask (the process mask
// the first time), 0 + ERROR_INVALID_PARAMETER if the mask is outside the
// process's (reference MMDevAPI.dll rva 0x81f0). FMOD treats 0 as failure: our
// old `return S_OK` made its WASAPI output init fail and the game fell back to
// its silent AudioEngineCommon, whose slot 84 returns null and crashes the
// world load (FINDINGS §4). STUB: the mask itself is left to the scheduler.
WINAPI uint64_t SetWasapiThreadAffinityMask_(uint64_t mask) {
    uint64_t process = kx::process_affinity_mask();
    // FL_NO_FMOD=1 (diagnostic): fail like the old stub, so FMOD's output init
    // fails and the game runs its silent AudioEngineCommon.
    if (getenv("FL_NO_FMOD")) return 0;
    if (mask & ~process) {
        kx::set_last_error(87);
        return 0;
    }
    static std::atomic<uint64_t> current{0};
    uint64_t prev = current.exchange(mask);
    FL_STUB_ONCE("SetWasapiThreadAffinityMask: mask recorded, not applied");
    return prev ? prev : process;
}

}  // namespace

HLE_EXPORT(mmdevapi, ActivateAudioInterfaceAsync, ActivateAudioInterfaceAsync_);
HLE_EXPORT(mmdevapi, SetWasapiThreadAffinityMask, SetWasapiThreadAffinityMask_);

// Called by combase's CoCreateInstance.
HRESULT create_mmdevice_enumerator(const GUID& clsid, const GUID& iid, void** out, bool* handled) {
    *handled = clsid == CLSID_MMDeviceEnumerator;
    if (!*handled) return E_NOINTERFACE;
    auto* e = new Enumerator();
    HRESULT hr = e->query(iid, out);
    e->release();
    return hr;
}

}  // namespace fl::audio
