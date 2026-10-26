#include "platform/input.h"

#include <SDL3/SDL.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <mutex>
#include <vector>

#include "common/log.h"

namespace fl::platform {
namespace {

// Windows.Xbox.Input.GamepadButtons (same values as the public
// Windows.Gaming.Input.GamepadButtons; unverified against the game).
enum : uint32_t {
    kMenu = 0x1, kView = 0x2, kA = 0x4, kB = 0x8, kX = 0x10, kY = 0x20,
    kDPadUp = 0x40, kDPadDown = 0x80, kDPadLeft = 0x100, kDPadRight = 0x200,
    kLeftShoulder = 0x400, kRightShoulder = 0x800, kLeftThumb = 0x1000, kRightThumb = 0x2000,
};

std::mutex g_mutex;
SDL_Gamepad* g_pad = nullptr;

SDL_Gamepad* pad() {
    if (g_pad && SDL_GamepadConnected(g_pad)) return g_pad;
    if (g_pad) SDL_CloseGamepad(g_pad), g_pad = nullptr;
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    if (ids && n > 0) {
        g_pad = SDL_OpenGamepad(ids[0]);
        if (g_pad) log_write("platform", "gamepad: %s", SDL_GetGamepadName(g_pad));
    }
    SDL_free(ids);
    return g_pad;
}

float axis(SDL_Gamepad* p, SDL_GamepadAxis a) {
    float v = SDL_GetGamepadAxis(p, a) / 32767.0f;
    return v < -1 ? -1 : v > 1 ? 1 : v;
}

// Keyboard fallback, for machines without a controller:
//   W/S = accelerate/brake (RT/LT), A/D = steer (left stick X),
//   arrows = D-pad (and left stick, for menus), Enter = A, Esc/Backspace = B,
//   Space = X, Y = Y, Q/E = LB/RB, Tab = View, P = Menu, IJKL = right stick.
void keyboard(PadState* s) {
    const bool* k = SDL_GetKeyboardState(nullptr);
    auto b = [&](SDL_Scancode c, uint32_t bit) {
        if (k[c]) s->buttons |= bit;
    };
    b(SDL_SCANCODE_RETURN, kA), b(SDL_SCANCODE_ESCAPE, kB), b(SDL_SCANCODE_BACKSPACE, kB);
    b(SDL_SCANCODE_SPACE, kX), b(SDL_SCANCODE_Y, kY), b(SDL_SCANCODE_Q, kLeftShoulder), b(SDL_SCANCODE_E, kRightShoulder);
    b(SDL_SCANCODE_TAB, kView), b(SDL_SCANCODE_P, kMenu);
    b(SDL_SCANCODE_UP, kDPadUp), b(SDL_SCANCODE_DOWN, kDPadDown), b(SDL_SCANCODE_LEFT, kDPadLeft),
        b(SDL_SCANCODE_RIGHT, kDPadRight);
    if (k[SDL_SCANCODE_W]) s->right_trigger = 1;
    if (k[SDL_SCANCODE_S]) s->left_trigger = 1;
    if (k[SDL_SCANCODE_A] || k[SDL_SCANCODE_LEFT]) s->lx = -1;
    if (k[SDL_SCANCODE_D] || k[SDL_SCANCODE_RIGHT]) s->lx = 1;
    if (k[SDL_SCANCODE_UP]) s->ly = 1;
    if (k[SDL_SCANCODE_DOWN]) s->ly = -1;
    if (k[SDL_SCANCODE_J]) s->rx = -1;
    if (k[SDL_SCANCODE_L]) s->rx = 1;
    if (k[SDL_SCANCODE_I]) s->ry = 1;
    if (k[SDL_SCANCODE_K]) s->ry = -1;
}

// FL_AUTOPRESS="35:A,37.5:Menu,40:A" holds a button for 250 ms at those
// seconds after start: lets test runs get past "press start" unattended.
// "T:Name/D" holds it for D seconds; RT/LT are the triggers (fully pressed),
// SteerL/SteerR the left stick (driving tests).
enum : uint32_t { kAutoRT = 1u << 24, kAutoLT = 1u << 25, kAutoSteerL = 1u << 26, kAutoSteerR = 1u << 27 };
struct AutoPress {
    double t;
    uint32_t bit;
    double hold = 0.25;
};
const std::vector<AutoPress>& autopress() {
    static std::vector<AutoPress> v = [] {
        std::vector<AutoPress> out;
        const char* env = getenv("FL_AUTOPRESS");
        if (!env) return out;
        static const struct { const char* name; uint32_t bit; } kNames[] = {
            {"A", kA}, {"B", kB}, {"X", kX}, {"Y", kY}, {"Menu", kMenu}, {"View", kView},
            {"Up", kDPadUp}, {"Down", kDPadDown}, {"Left", kDPadLeft}, {"Right", kDPadRight},
            {"LB", kLeftShoulder}, {"RB", kRightShoulder}, {"RT", kAutoRT}, {"LT", kAutoLT},
            {"SteerL", kAutoSteerL}, {"SteerR", kAutoSteerR}};
        std::string spec(env);
        size_t pos = 0;
        while (pos < spec.size()) {
            size_t end = spec.find(',', pos);
            std::string item = spec.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? spec.size() : end + 1;
            size_t colon = item.find(':');
            if (colon == std::string::npos) continue;
            double t = atof(item.substr(0, colon).c_str());
            std::string name = item.substr(colon + 1);
            double hold = 0.25;
            if (size_t slash = name.find('/'); slash != std::string::npos) {
                hold = atof(name.substr(slash + 1).c_str());
                name = name.substr(0, slash);
            }
            for (auto& n : kNames)
                if (name == n.name) out.push_back({t, n.bit, hold});
        }
        log_write("platform", "FL_AUTOPRESS: %zu scripted presses", out.size());
        return out;
    }();
    return v;
}

uint32_t autopress_buttons() {
    double now = SDL_GetTicksNS() / 1e9;
    uint32_t b = 0;
    for (auto& a : autopress())
        if (now >= a.t && now < a.t + a.hold) b |= a.bit;
    return b;
}

}  // namespace

PadState read_pad() {
    std::lock_guard lock(g_mutex);
    PadState s{};
    s.timestamp = SDL_GetTicksNS() / 100;
    if (SDL_Gamepad* p = pad()) {
        auto b = [&](SDL_GamepadButton btn, uint32_t bit) {
            if (SDL_GetGamepadButton(p, btn)) s.buttons |= bit;
        };
        b(SDL_GAMEPAD_BUTTON_SOUTH, kA), b(SDL_GAMEPAD_BUTTON_EAST, kB), b(SDL_GAMEPAD_BUTTON_WEST, kX),
            b(SDL_GAMEPAD_BUTTON_NORTH, kY);
        b(SDL_GAMEPAD_BUTTON_START, kMenu), b(SDL_GAMEPAD_BUTTON_BACK, kView);
        b(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kLeftShoulder), b(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kRightShoulder);
        b(SDL_GAMEPAD_BUTTON_LEFT_STICK, kLeftThumb), b(SDL_GAMEPAD_BUTTON_RIGHT_STICK, kRightThumb);
        b(SDL_GAMEPAD_BUTTON_DPAD_UP, kDPadUp), b(SDL_GAMEPAD_BUTTON_DPAD_DOWN, kDPadDown),
            b(SDL_GAMEPAD_BUTTON_DPAD_LEFT, kDPadLeft), b(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kDPadRight);
        s.left_trigger = axis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
        s.right_trigger = axis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        s.lx = axis(p, SDL_GAMEPAD_AXIS_LEFTX);
        s.ly = -axis(p, SDL_GAMEPAD_AXIS_LEFTY);  // SDL: +y down
        s.rx = axis(p, SDL_GAMEPAD_AXIS_RIGHTX);
        s.ry = -axis(p, SDL_GAMEPAD_AXIS_RIGHTY);
    }
    keyboard(&s);  // the keyboard works alongside a gamepad too
    uint32_t auto_b = autopress_buttons();
    if (auto_b & kAutoRT) s.right_trigger = 1;
    if (auto_b & kAutoLT) s.left_trigger = 1;
    if (auto_b & kAutoSteerL) s.lx = -1;
    if (auto_b & kAutoSteerR) s.lx = 1;
    s.buttons |= auto_b & 0xffffff;
    static uint32_t last = 0;
    if (s.buttons != last) {
        FL_LOG(input, "buttons 0x%x", s.buttons);
        last = s.buttons;
    }
    return s;
}

void set_vibration(float left, float right, float lt, float rt) {
    std::lock_guard lock(g_mutex);
    if (SDL_Gamepad* p = pad()) {
        auto u16 = [](float v) { return static_cast<Uint16>((v < 0 ? 0 : v > 1 ? 1 : v) * 65535); };
        SDL_RumbleGamepad(p, u16(left), u16(right), 100);
        SDL_RumbleGamepadTriggers(p, u16(lt), u16(rt), 100);
    }
}

}  // namespace fl::platform
