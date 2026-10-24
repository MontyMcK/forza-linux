#pragma once

namespace fl::crt {

// MSVC std::exception layout (x64, VS2012).
struct StdException {
    void* const* vft;
    const char* what;
    bool dofree;
};
static_assert(sizeof(StdException) == 24);

// Construct an std::exception in place (vftable = std::exception's).
// Derived types overwrite vft afterwards.
void std_exception_init(StdException* e, const char* msg);

}  // namespace fl::crt
