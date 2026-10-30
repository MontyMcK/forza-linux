#!/usr/bin/env bash
# The PS5 C/C++ compiler: PS5_Vulkan's prospero-clang18 wrapper over the pinned
# payload SDK fork (tools/ps5/setup_deps.sh). Arguments go to clang.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
vulkan="$root/build-ps5/deps/PS5_Vulkan"
export PS5_PAYLOAD_SDK="$vulkan/.deps/native/ps5-payload-sdk"
[[ -x $PS5_PAYLOAD_SDK/bin/prospero-lld ]] || { echo "run tools/ps5/setup_deps.sh first" >&2; exit 2; }
exec sh "$vulkan/tooling/prospero-clang18" "$@"
