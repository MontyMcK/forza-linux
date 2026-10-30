#!/usr/bin/env bash
# Builds PS5 targets into build-ps5/ (gitignored).
#   tools/ps5/build.sh probe     the platform probe payload (build-ps5/probe/oh2probe.elf)
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
  *) echo "unknown target: $target" >&2; exit 2 ;;
esac
