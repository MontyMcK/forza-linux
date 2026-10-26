cmake_minimum_required(VERSION 3.25)

# clang is required: we rely on __attribute__((ms_abi)) and __builtin_ms_va_list.
if(NOT CMAKE_CXX_COMPILER)
  set(CMAKE_C_COMPILER clang)
  set(CMAKE_CXX_COMPILER clang++)
endif()

project(forza_linux LANGUAGES C CXX ASM)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "forza-linux must be built with clang (ms_abi support)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# No C++ exceptions in our code: they must never unwind through game frames.
# -mno-red-zone is not needed (we never run inside game signal frames yet).
add_compile_options(-Wall -Wextra -Wno-unused-parameter -fno-exceptions -fno-strict-aliasing)

file(GLOB_RECURSE FL_SOURCES CONFIGURE_DEPENDS
  src/common/*.cpp
  src/loader/*.cpp
  src/loader/*.S
  src/hle/*.cpp
  src/hle/*.S
  src/platform/*.cpp)

# AMD addrlib (MIT, from Mesa; third_party/addrlib/MESA_COMMIT): GCN surface layouts.
file(GLOB_RECURSE FL_ADDRLIB_SOURCES third_party/addrlib/src/*.cpp)
add_library(addrlib STATIC ${FL_ADDRLIB_SOURCES})
target_include_directories(addrlib PUBLIC third_party/addrlib/inc PRIVATE third_party/addrlib/src
  third_party/addrlib/src/core third_party/addrlib/src/chip/r800 third_party/addrlib/src/chip/gfx9
  third_party/addrlib/src/chip/gfx10 third_party/addrlib/src/chip/gfx11 third_party/addrlib/src/chip/gfx12)
target_compile_definitions(addrlib PRIVATE ADDR_FASTCALL= LITTLEENDIAN_CPU DEBUG=0 ADDR_ALLOW_SIMD=1)
target_compile_options(addrlib PRIVATE -w -fexceptions)

add_executable(forza-linux ${FL_SOURCES})
target_include_directories(forza-linux PRIVATE src)
# Developer-only shortcut: FL_SKIP_MOVIES=<name,...> makes the named Bink movies
# "not found" so test boots skip the long intro. Must stay OFF in the repo and
# in any build that is shared; enable it only in a local build directory.
option(FL_DEV_SKIP_MOVIES "dev only: honour FL_SKIP_MOVIES (never in shared builds)" OFF)
if(FL_DEV_SKIP_MOVIES)
  message(WARNING "FL_DEV_SKIP_MOVIES is ON: developer build, do not share it")
  target_compile_definitions(forza-linux PRIVATE FL_DEV_SKIP_MOVIES=1)
endif()
# dxvk-native (D3D11 + DXGI on Vulkan), built by tools/build_dxvk.sh.
# Only the libraries are linked into forza-linux: DXVK's Windows headers clash
# with src/common/winabi.h, so src/hle/d3d11x calls DXVK through raw vtables.
set(FL_DXVK_ROOT ${CMAKE_SOURCE_DIR}/build-dxvk/install)
if(NOT EXISTS ${FL_DXVK_ROOT}/lib64/libdxvk_d3d11.so)
  message(FATAL_ERROR "dxvk-native is not built yet: run tools/build_dxvk.sh")
endif()
set(FL_DXVK_LIBS ${FL_DXVK_ROOT}/lib64/libdxvk_d3d11.so ${FL_DXVK_ROOT}/lib64/libdxvk_dxgi.so)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
pkg_check_modules(ZLIB REQUIRED IMPORTED_TARGET zlib)
target_link_libraries(forza-linux PRIVATE pthread dl ${FL_DXVK_LIBS} PkgConfig::SDL3 PkgConfig::ZLIB addrlib)
# HLE exports register themselves through static constructors; keep them all.
target_link_options(forza-linux PRIVATE -Wl,--export-dynamic)

# Optional: FFmpeg's XMA decoders (tools/build_ffmpeg_xma.sh) for the game's
# XMA audio (src/hle/acphal/shape.cpp). Without them XMA voices are silent.
set(FL_FFMPEG_XMA_ROOT ${CMAKE_SOURCE_DIR}/build-ffmpeg-xma/install)
if(EXISTS ${FL_FFMPEG_XMA_ROOT}/lib/libavcodec_flxma.so)
  message(STATUS "XMA audio: FFmpeg decoders from ${FL_FFMPEG_XMA_ROOT}")
  target_compile_definitions(forza-linux PRIVATE FL_HAVE_FFMPEG_XMA=1)
  target_include_directories(forza-linux SYSTEM PRIVATE ${FL_FFMPEG_XMA_ROOT}/include)
  target_link_libraries(forza-linux PRIVATE ${FL_FFMPEG_XMA_ROOT}/lib/libavcodec_flxma.so
                        ${FL_FFMPEG_XMA_ROOT}/lib/libavutil_flxma.so)
  set_target_properties(forza-linux PROPERTIES BUILD_RPATH ${FL_FFMPEG_XMA_ROOT}/lib)
else()
  message(STATUS "XMA audio: no FFmpeg decoders (run tools/build_ffmpeg_xma.sh), XMA voices will be silent")
endif()

add_executable(dxvk_smoke tests/dxvk_smoke.cpp)
target_include_directories(dxvk_smoke SYSTEM PRIVATE ${FL_DXVK_ROOT}/include/dxvk)
target_link_libraries(dxvk_smoke PRIVATE ${FL_DXVK_LIBS})

add_executable(gcn_layout_test tests/gcn_layout_test.cpp src/hle/xg/gcn.cpp src/common/log.cpp)
target_include_directories(gcn_layout_test PRIVATE src)
target_link_libraries(gcn_layout_test PRIVATE addrlib pthread)

# Dev tool (reads game files): untile a .swatchbin mip to PNG.
add_executable(swatch_view tools/swatch_view.cpp src/hle/xg/gcn.cpp src/common/log.cpp src/common/png.cpp)
target_include_directories(swatch_view PRIVATE src)
target_link_libraries(swatch_view PRIVATE addrlib pthread PkgConfig::ZLIB)
