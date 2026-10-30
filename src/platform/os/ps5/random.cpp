#include "platform/os/random.h"

#include <stdlib.h>

namespace fl::os {

// FreeBSD's arc4random_buf (PS5_Vulkan's link recipe binds it to the platform layer's).
void random_bytes(void* buf, size_t len) { arc4random_buf(buf, len); }

}  // namespace fl::os
