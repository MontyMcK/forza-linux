#include "platform/os/random.h"

#include <cstdint>
#include <sys/random.h>

namespace fl::os {

void random_bytes(void* buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t r = getrandom(static_cast<uint8_t*>(buf) + done, len - done, 0);
        if (r > 0) done += static_cast<size_t>(r);
    }
}

}  // namespace fl::os
