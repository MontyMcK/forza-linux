#!/usr/bin/env bash
# Builds PS5 targets into build-ps5/ (gitignored).
#   tools/ps5/build.sh probe     the platform probe payload (build-ps5/probe/oh2probe.elf)
#   tools/ps5/build.sh copydump  copies the user's dump from USB to /data/OpenHorizon2 on the console
#   tools/ps5/build.sh probe-title  the probe as the title's eboot, packaged (needs tools/ps5/setup_deps.sh)
#   tools/ps5/build.sh launch    payloads that start the title / report what runs (build-ps5/launch/)
#   tools/ps5/build.sh os-test-title  tests/os_test.cpp on the PS5 backend, as the title's eboot
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
