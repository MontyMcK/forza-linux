# CMake toolchain for the PS5 title (x86_64-sie-ps5): wraps the payload SDK
# fork's prospero.cmake that tools/ps5/setup_deps.sh installs (docs/PS5_PORT.md).
#   tools/ps5/setup_deps.sh
#   cmake -S . -B build-ps5/cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/ps5.cmake
get_filename_component(OH2_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(OH2_PS5_VULKAN "${OH2_ROOT}/build-ps5/deps/PS5_Vulkan")
set(OH2_PS5_SDK "${OH2_PS5_VULKAN}/.deps/native/ps5-payload-sdk")
if(NOT EXISTS "${OH2_PS5_SDK}/toolchain/prospero.cmake")
  message(FATAL_ERROR "PS5 SDK not set up: run tools/ps5/setup_deps.sh")
endif()
include("${OH2_PS5_SDK}/toolchain/prospero.cmake")

# The PS5 CPU is Zen 2 (AVX2, FMA, BMI2): build for it, not the host.
set(CMAKE_C_FLAGS_INIT "-march=znver2")
set(CMAKE_CXX_FLAGS_INIT "-march=znver2")

# Nothing from the host: pkg-config would add /usr/include.
set(ENV{PKG_CONFIG_LIBDIR} "/nonexistent-pkg-config-for-ps5")
set(ENV{PKG_CONFIG_PATH} "")

# CMake's C++20 module scan runs the host clang-scan-deps, which never sees
# the SDK's libc++ headers; we use no modules.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# src/platform/os/ps5/ is the OS backend.
set(FL_OS ps5 CACHE STRING "OS backend: src/platform/os/<FL_OS>/" FORCE)
