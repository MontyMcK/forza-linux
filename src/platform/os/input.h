// Gamepad input (Linux: the first SDL gamepad, or the keyboard when none is
// plugged in). Buttons use the Xbox GamepadButtons bit values.
#pragma once

#include <cstdint>

namespace fl::os {

struct PadState {
    uint64_t timestamp;  // 100 ns ticks
    uint32_t buttons;
    float left_trigger, right_trigger;  // 0..1
    float lx, ly, rx, ry;               // -1..1, +y = up
};

PadState read_pad();
void set_vibration(float left, float right, float left_trigger, float right_trigger);

}  // namespace fl::os
