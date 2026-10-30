// PS5 controller: the initial user's DualSense through ScePad, mapped onto the
// Xbox GamepadButtons like the Linux backend maps SDL's buttons. The sample
// layout and button bits are the ones PS5_MihawkOS reads (ps5/src/platform.c,
// platform.h, MIT; FINDINGS 7.9). Trigger rumble (adaptive triggers) is M8.
#include "platform/os/input.h"

#include <cstddef>
#include <cstring>
#include <ctime>
#include <mutex>

#include "common/log.h"

extern "C" {
int sceUserServiceInitialize(const void* params);
int sceUserServiceGetInitialUser(int32_t* user_id);
int scePadInit(void);
int scePadOpen(int32_t user_id, int32_t port_type, int32_t index, const void* params);
int scePadRead(int32_t handle, void* samples, int32_t capacity);
int scePadSetVibrationMode(int32_t handle, int32_t mode);
int scePadSetVibration(int32_t handle, const void* motors);
}

namespace fl::os {
namespace {

// One sample as the console writes it (120 bytes).
struct PadSample {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry, l2, r2;  // sticks 0..255, 128 at rest, +y down
    uint16_t reserved0;
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touch_count, reserved1[3];
    uint32_t reserved2;
    struct {
        uint16_t x, y;
        uint8_t id, reserved[3];
    } touches[2];
    int32_t connected;
    uint64_t timestamp_us;
    uint8_t extension[16];
    uint8_t connected_count;
    uint8_t remaining[15];
};
static_assert(sizeof(PadSample) == 120 && offsetof(PadSample, connected) == 0x4c &&
              offsetof(PadSample, timestamp_us) == 0x50);

// DualSense buttons (the console's numbering).
enum : uint32_t {
    kPsL3 = 0x2, kPsR3 = 0x4, kPsOptions = 0x8, kPsUp = 0x10, kPsRight = 0x20, kPsDown = 0x40, kPsLeft = 0x80,
    kPsL1 = 0x400, kPsR1 = 0x800, kPsTriangle = 0x1000, kPsCircle = 0x2000, kPsCross = 0x4000,
    kPsSquare = 0x8000, kPsTouchPad = 0x100000,
    kPsIntercepted = 0x80000000,  // the shell has the pad (home screen, a system dialog)
};

// Windows.Xbox.Input.GamepadButtons (as in the Linux backend).
enum : uint32_t {
    kMenu = 0x1, kView = 0x2, kA = 0x4, kB = 0x8, kX = 0x10, kY = 0x20,
    kDPadUp = 0x40, kDPadDown = 0x80, kDPadLeft = 0x100, kDPadRight = 0x200,
    kLeftShoulder = 0x400, kRightShoulder = 0x800, kLeftThumb = 0x1000, kRightThumb = 0x2000,
};

constexpr int32_t kVibrationCompatible = 2;  // two motors, large and small

std::mutex g_mutex;
int32_t g_handle = -1;
double g_next_try = 0;
PadSample g_last;
uint8_t g_motors[2];

double now_s() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

void neutral(PadSample* s) {
    memset(s, 0, sizeof *s);
    s->lx = s->ly = s->rx = s->ry = 128;
}

// Opens the initial user's pad; retried once a second while it fails (a
// title can start before the pad service has published the device).
bool open_pad() {
    if (g_handle >= 0) return true;
    double now = now_s();
    if (now < g_next_try) return false;
    g_next_try = now + 1;
    static bool init = [] {
        sceUserServiceInitialize(nullptr);  // fails harmlessly when already initialised
        int rc = scePadInit();
        if (rc < 0) log_write("platform", "pad: scePadInit failed (0x%x)", rc);
        return rc >= 0;
    }();
    int32_t user = -1;
    if (!init || sceUserServiceGetInitialUser(&user) < 0) return false;
    g_handle = scePadOpen(user, 0, 0, nullptr);
    static bool reported = false;
    if (g_handle >= 0) {
        neutral(&g_last);
        log_write("platform", "pad: opened for user 0x%x (rumble mode: 0x%x)", user,
                  scePadSetVibrationMode(g_handle, kVibrationCompatible));
    } else if (!reported) {
        reported = true;
        log_write("platform", "pad: scePadOpen for user 0x%x failed (0x%x), retrying", user, g_handle);
    }
    return g_handle >= 0;
}

float stick(uint8_t v) {
    float f = (v - 128.0f) / 127.0f;
    return f < -1 ? -1 : f > 1 ? 1 : f;
}

}  // namespace

PadState read_pad() {
    std::lock_guard lock(g_mutex);
    PadState s{};
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    s.timestamp = (ts.tv_sec * 1'000'000'000ull + ts.tv_nsec) / 100;
    if (!open_pad()) return s;
    // Drain the queue and keep the newest sample; a press that came and went
    // since the last call still counts for this one.
    static PadSample samples[64];
    uint32_t seen = 0;
    int n = scePadRead(g_handle, samples, 64);
    if (n < 0) neutral(&g_last);
    for (int i = 0; i < n && i < 64; i++) {
        g_last = samples[i];
        if (!g_last.connected || (g_last.buttons & kPsIntercepted)) neutral(&g_last);
        seen |= g_last.buttons;
    }
    uint32_t ps = g_last.buttons | seen;
    auto map = [&](uint32_t from, uint32_t to) {
        if (ps & from) s.buttons |= to;
    };
    map(kPsCross, kA), map(kPsCircle, kB), map(kPsSquare, kX), map(kPsTriangle, kY);
    map(kPsOptions, kMenu), map(kPsTouchPad, kView);
    map(kPsL1, kLeftShoulder), map(kPsR1, kRightShoulder), map(kPsL3, kLeftThumb), map(kPsR3, kRightThumb);
    map(kPsUp, kDPadUp), map(kPsDown, kDPadDown), map(kPsLeft, kDPadLeft), map(kPsRight, kDPadRight);
    s.left_trigger = g_last.l2 / 255.0f;
    s.right_trigger = g_last.r2 / 255.0f;
    s.lx = stick(g_last.lx);
    s.ly = -stick(g_last.ly);  // the console: +y down
    s.rx = stick(g_last.rx);
    s.ry = -stick(g_last.ry);
    static uint32_t last = 0;
    if (s.buttons != last) {
        FL_LOG(input, "buttons 0x%x", s.buttons);
        last = s.buttons;
    }
    return s;
}

void set_vibration(float left, float right, float lt, float rt) {
    std::lock_guard lock(g_mutex);
    if (lt > 0 || rt > 0) FL_STUB_ONCE("os::set_vibration on PS5: trigger rumble ignored (adaptive triggers, M8)");
    if (!open_pad()) return;
    auto u8 = [](float v) { return static_cast<uint8_t>((v < 0 ? 0 : v > 1 ? 1 : v) * 255); };
    uint8_t motors[2] = {u8(left), u8(right)};  // Xbox left = the large motor
    if (!memcmp(motors, g_motors, 2)) return;
    memcpy(g_motors, motors, 2);
    scePadSetVibration(g_handle, motors);
}

}  // namespace fl::os
