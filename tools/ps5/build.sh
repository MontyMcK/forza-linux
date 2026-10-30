#!/usr/bin/env bash
# Builds PS5 targets into build-ps5/ (gitignored).
#   tools/ps5/build.sh probe     the platform probe payload (build-ps5/probe/oh2probe.elf)
#   tools/ps5/build.sh copydump  copies the user's dump from USB to /data/OpenHorizon2 on the console
#   tools/ps5/build.sh probe-title  the probe as the title's eboot, packaged (needs tools/ps5/setup_deps.sh)
#   tools/ps5/build.sh launch    payloads that start the title / report what runs (build-ps5/launch/)
#   tools/ps5/build.sh os-test-title  tests/os_test.cpp on the PS5 backend, as the title's eboot
#   tools/ps5/build.sh runtime   forza-linux for the PS5, as the title's eboot
set -euo pipefail
source "$(dirname "$0")/lib.sh"
target=${1:-probe}
[[ -x $PS5_PAYLOAD_SDK/bin/prospero-clang ]] || { echo "no payload SDK at $PS5_PAYLOAD_SDK" >&2; exit 2; }
case $target in
  probe)
    out="$ps5_root/build-ps5/probe"
    mkdir -p "$out"
    "$PS5_PAYLOAD_SDK/bin/prospero-clang" -std=gnu11 -O1 -Wall -Werror -lpthread \
      -o "$out/oh2probe.elf" "$ps5_root/tools/ps5/probe/oh2probe.c"
    echo "$out/oh2probe.elf" ;;
  copydump)
    out="$ps5_root/build-ps5/copydump"
    mkdir -p "$out"
    "$PS5_PAYLOAD_SDK/bin/prospero-clang" -std=gnu11 -O1 -Wall -Werror \
      -o "$out/oh2copydump.elf" "$ps5_root/tools/ps5/copydump/oh2copydump.c"
    echo "$out/oh2copydump.elf" ;;
  probe-title)
    # The probe as the title's eboot, packaged as build-ps5/title/$PS5_TITLE_ID
    out="$ps5_root/build-ps5/probe-title"
    mkdir -p "$out"
    netlog=()
    [[ -n ${PS5_LOG_HOST:-} ]] && netlog=(-DOH2_LOG_HOST=\"$PS5_LOG_HOST\" -DOH2_LOG_PORT="${PS5_LOG_PORT:-9599}")
    "$ps5_root/tools/ps5/cc.sh" -std=gnu11 -O1 -Wall -Werror -DOH2_PROBE_TITLE "${netlog[@]}" \
      -c "$ps5_root/tools/ps5/probe/oh2probe.c" -o "$out/oh2probe.o"
    "$ps5_root/tools/ps5/link_title.sh" "$out/eboot.bin" "$out/oh2probe.o"
    "$ps5_root/tools/ps5/package.sh" "$out/eboot.bin" ;;
  os-test-title)
    # tests/os_test.cpp on the PS5 backend, packaged as the title's eboot;
    # its output goes to tools/ps5/netlog.py (PS5_LOG_HOST).
    out="$ps5_root/build-ps5/os-test"
    mkdir -p "$out"
    objs=()
    for src in "$ps5_root"/tests/os_test.cpp "$ps5_root"/src/platform/os/ps5/*.cpp "$ps5_root"/src/common/log.cpp; do
      obj="$out/$(basename "${src%.cpp}").o"
      "$ps5_root/tools/ps5/cc.sh" -std=c++20 -O2 -Wall -Werror -Wno-unused-parameter -fno-exceptions \
        -DOH2_LOG_HOST=\"$PS5_LOG_HOST\" -DOH2_LOG_PORT="${PS5_LOG_PORT:-9599}" -I"$ps5_root/src" -c "$src" -o "$obj"
      objs+=("$obj")
    done
    "$ps5_root/tools/ps5/link_title.sh" "$out/eboot.bin" "${objs[@]}"
    "$ps5_root/tools/ps5/package.sh" "$out/eboot.bin" ;;
  runtime)
    # forza-linux for the PS5 as the title's eboot (PS5_PORT.md Step 6, M1).
    # Our code: tools/ps5/cc-win.sh (Windows-ABI capable); zlib and addrlib:
    # the PS5 target. Every runtime object is linked (HLE exports register
    # through static constructors).
    out="$ps5_root/build-ps5/runtime"
    mkdir -p "$out/obj" "$out/zlib" "$out/addrlib"
    sdkbin="$ps5_root/build-ps5/deps/PS5_Vulkan/.deps/native/ps5-payload-sdk/bin"
    zsrc=$(ls -d "$ps5_root"/build-ps5/deps/PS5_Vulkan/.deps/native/zlib/zlib-*/ | head -1)
    jobs=$(nproc)
    export ps5_root zsrc out
    printf '%s\n' adler32 compress crc32 deflate gzclose gzlib gzread gzwrite infback inffast inflate inftrees trees uncompr zutil |
      xargs -P "$jobs" -I{} "$ps5_root/tools/ps5/cc.sh" -std=c11 -O2 -w -c "$zsrc/{}.c" -o "$out/zlib/{}.o"
    rm -f "$out/libz.a" && "$sdkbin/llvm-ar" rcs "$out/libz.a" "$out"/zlib/*.o
    dxvk="$ps5_root/build-ps5/dxvk/install/lib"
    radv="$ps5_root/build-ps5/deps/PS5_Vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a"
    use_radv=0
    [[ -f $dxvk/libdxvk_d3d11.a && -f $radv ]] && use_radv=1
    # RADV (linked whole) carries Mesa's addrlib: with it, use that copy and
    # its headers instead of third_party/addrlib (the symbols would clash).
    addr_inc="$ps5_root/third_party/addrlib/inc"
    addr_lib=()
    if (( use_radv )); then
      addr_inc="$ps5_root/build-ps5/deps/PS5_Mesa/src/amd/addrlib/inc"
    else
      a="$ps5_root/third_party/addrlib"
      find "$a/src" -name '*.cpp' | xargs -P "$jobs" -I{} sh -c '"$ps5_root/tools/ps5/cc.sh" -std=c++20 -O2 -w -fexceptions \
        -DADDR_FASTCALL= -DLITTLEENDIAN_CPU -DDEBUG=0 -DADDR_ALLOW_SIMD=1 -I"$0/inc" -I"$0/src" -I"$0/src/core" \
        -I"$0/src/chip/r800" -I"$0/src/chip/gfx9" -I"$0/src/chip/gfx10" -I"$0/src/chip/gfx11" -I"$0/src/chip/gfx12" \
        -c "$1" -o "$out/addrlib/$(basename "$1" .cpp).o"' "$a" {}
      rm -f "$out/libaddrlib.a" && "$sdkbin/llvm-ar" rcs "$out/libaddrlib.a" "$out"/addrlib/*.o
      addr_lib=("$out/libaddrlib.a")
    fi
    export addr_inc
    find "$ps5_root/src" \( -name '*.cpp' -o -name '*.S' \) -not -path '*/platform/os/linux/*' | sort > "$out/sources.txt"
    xargs -P "$jobs" -I{} sh -c 'o="$out/obj/$(echo "$1" | sed "s|$ps5_root/src/||; s|/|_|g").o"
      "$ps5_root/tools/ps5/cc-win.sh" -std=c++20 -O2 -g -Wall -Wextra -Wno-unused-parameter -fno-exceptions -fno-strict-aliasing \
        -I"$ps5_root/src" -I"$addr_inc" -I"$zsrc" -c "$1" -o "$o"' _ {} < "$out/sources.txt"
    if (( use_radv )); then
      # DXVK (tools/ps5/build_dxvk.sh) on PS5_Vulkan's RADV
      gfx=("$dxvk"/libdxvk_d3d11.a "$dxvk"/libdxvk_dxgi.a "$dxvk"/libdxvk.a "$dxvk"/libwsi.a "$dxvk"/libvkcommon.a
           "$dxvk"/libutil.a "$dxvk"/libspirv.a "$dxvk"/libdxbc_spv.a "$dxvk"/libdisplay-info.a)
      export RADV_ARCHIVE="$radv"
    else
      # No PS5 DXVK yet: its entry points fail loudly (tools/ps5/m1/no_dxvk.cpp).
      "$ps5_root/tools/ps5/cc-win.sh" -std=c++20 -O2 -fno-exceptions -I"$ps5_root/src" \
        -c "$ps5_root/tools/ps5/m1/no_dxvk.cpp" -o "$out/no_dxvk.o"
      gfx=("$out/no_dxvk.o")
    fi
    # RADV's archive also carries zlib (1.3.1): with it, the title takes that copy.
    zlib=("$out/libz.a")
    (( use_radv )) && zlib=()
    "$ps5_root/tools/ps5/link_title.sh" "$out/eboot.bin" "$out"/obj/*.o "${gfx[@]}" "${addr_lib[@]}" "${zlib[@]}"
    "$ps5_root/tools/ps5/package.sh" "$out/eboot.bin" ;;
  launch)
    out="$ps5_root/build-ps5/launch"
    mkdir -p "$out"
    "$PS5_PAYLOAD_SDK/bin/prospero-clang" -std=gnu11 -O1 -Wall -Werror -DOH2_TITLE_ID=\"$PS5_TITLE_ID\" \
      -lSceSystemService -lSceUserService -o "$out/oh2launch.elf" "$ps5_root/tools/ps5/launch/oh2launch.c"
    "$PS5_PAYLOAD_SDK/bin/prospero-clang" -std=gnu11 -O1 -Wall -Werror -DOH2_TITLE_ID=\"$PS5_TITLE_ID\" -DOH2_STATUS_ONLY \
      -lSceSystemService -lSceUserService -o "$out/oh2status.elf" "$ps5_root/tools/ps5/launch/oh2launch.c"
    echo "$out/oh2launch.elf $out/oh2status.elf" ;;
  *) echo "unknown target: $target" >&2; exit 2 ;;
esac
