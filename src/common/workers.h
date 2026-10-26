// A small pool of host worker threads for CPU-heavy runtime work (texture
// untiling). They never run game code.
#pragma once

#include <cstddef>
#include <functional>

namespace fl {

// Runs fn(i) for every i in [0, n) on the pool and the calling thread;
// returns when all are done. Calls from several threads at once are fine
// (they take turns with the pool).
void parallel_for(size_t n, const std::function<void(size_t)>& fn);

}  // namespace fl
