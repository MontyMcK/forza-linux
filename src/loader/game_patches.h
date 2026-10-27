// Small, checked patches to the game's own code (FINDINGS §6c).
#pragma once

#include "loader/pe_image.h"

namespace fl::loader {

// Applies the patches the configuration asks for. Call after map_image and
// before protect_image (the image is still writable).
void apply_game_patches(PeImage& img);

}  // namespace fl::loader
