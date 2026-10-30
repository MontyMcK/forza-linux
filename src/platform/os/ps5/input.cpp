// PS5 controller: ScePad, not written yet (PS5_PORT.md M4).
#include "platform/os/input.h"

#include "common/log.h"

namespace fl::os {

PadState read_pad() {
    FL_STUB_ONCE("UNIMPLEMENTED os::read_pad on PS5 (ScePad, PS5_PORT.md M4): no input");
    return {};
}

void set_vibration(float, float, float, float) {
    FL_STUB_ONCE("UNIMPLEMENTED os::set_vibration on PS5 (ScePad): no rumble");
}

}  // namespace fl::os
