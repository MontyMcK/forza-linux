#!/usr/bin/env bash
# Fetches the pinned PS5 dependencies into build-ps5/deps/ (gitignored) and
# prepares PS5_Vulkan's title toolchain: its payload SDK fork, zlib, the
# clean-room libc.prx (hash-checked) and the ps5-native-tool host tool.
# Pins and what each is for: docs/PS5_PORT.md (Dependencies).
set -euo pipefail
source "$(dirname "$0")/lib.sh"
deps="$ps5_root/build-ps5/deps"
mkdir -p "$deps"
fetch() {  # repo sha dir
  local repo=$1 sha=$2 dir="$deps/$3"
  if [[ ! -d $dir/.git ]]; then
    git init -q "$dir"
    git -C "$dir" remote add origin "https://github.com/$repo"
  fi
  if [[ $(git -C "$dir" rev-parse HEAD 2>/dev/null) != "$sha" ]]; then
    git -C "$dir" fetch -q --depth 1 origin "$sha"
    git -C "$dir" checkout -q FETCH_HEAD
  fi
  echo "$3 at ${sha:0:12}"
}
fetch mihawk-99/PS5_Vulkan f410e67a0241b3e8edcdbcd2a9b285dfc4a412ed PS5_Vulkan
# PS5_Vulkan's own pins (tools/setup-native-dependencies.sh, tools/build-radv.sh)
fetch mihawk-99/PS5_PayloadSDK b83202be73e930050e00de0f1b4d9ec46c0391bf PS5_PayloadSDK
fetch mihawk-99/PS5_Mesa 1ba9874f478657ccf6da72ce55f0260164cc0bd0 PS5_Mesa
cd "$deps/PS5_Vulkan"
bash tools/setup-native-dependencies.sh > /dev/null
[[ -f runtime/libc.prx ]] || make libc
(cd runtime && sha256sum --check --strict --quiet libc.prx.sha256)
# Their skeleton build compiles the ps5-native-tool host tool; the skeleton
# needs the platform layer, which this revision's build.sh doesn't add itself.
[[ -x build/host/ps5-native-tool ]] ||
  make APP_STATIC_ARCHIVES=.deps/native/ps5-payload-sdk/target/lib/libps5platform.a > /dev/null
echo "PS5 title toolchain ready in $deps/PS5_Vulkan"
