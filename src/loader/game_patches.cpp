#include "loader/game_patches.h"

#include <cstdlib>
#include <cstring>

#include "common/config.h"
#include "common/log.h"

namespace fl::loader {
namespace {

// Replaces `n` bytes at `rva` after checking they are the expected original
// ones: a different game build fails loudly instead of being corrupted.
void patch(PeImage& img, const char* what, uint32_t rva, const uint8_t* expect, const uint8_t* with, size_t n) {
    uint8_t* p = img.at<uint8_t>(rva);
    if (std::memcmp(p, expect, n) != 0) fatal("game patch '%s' at rva 0x%x: unexpected bytes (other game build?)", what, rva);
    std::memcpy(p, with, n);
    log_write("loader", "game patch: %s (rva 0x%x)", what, rva);
}

}  // namespace

void apply_game_patches(PeImage& img) {
    if (config().pacing == Config::Pacing::dynamic) {
        // The update step (rva 0xf58770) is the measured frame time (clamped to
        // 0.2 s) unless synchroniseUpdateToRenderThread (+0x2dab) is on, the
        // console default, which makes it a fixed 1/29.97 s. The retail game
        // never reads that option from its command line, so the branch is
        // forced: `je` (skip the fixed step when the option is off) -> `jmp`.
        static const uint8_t je[] = {0x74, 0x1e}, jmp[] = {0xeb, 0x1e};
        patch(img, "real-time update step", 0xf587dd, je, jmp, sizeof je);
    }
}

}  // namespace fl::loader
