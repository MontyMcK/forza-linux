#!/usr/bin/env bash
# Builds dxvk-native (D3D11 + DXGI, SDL3 WSI) from the third_party/dxvk
# submodule and installs it into build-dxvk/install (gitignored).
# Needs: meson, glslang, SDL3-devel, vulkan-headers, vulkan-loader-devel.
# Usage: tools/build_dxvk.sh
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
git submodule update --init --recursive third_party/dxvk
# Our patches on top of the pinned release (tools/dxvk-patches/*.patch), applied
# in order. If the last one already reverse-applies, the series is in place;
# otherwise the submodule's sources are reset and the whole series applied.
patches=(tools/dxvk-patches/*.patch)
if [ -e "${patches[0]}" ]; then
  last="${patches[${#patches[@]}-1]}"
  if git -C third_party/dxvk apply --reverse --check "$root/$last" 2>/dev/null; then
    echo "dxvk patches already applied (${#patches[@]})"
  else
    git -C third_party/dxvk checkout -- src
    for p in "${patches[@]}"; do
      git -C third_party/dxvk apply "$root/$p"
      echo "dxvk patch applied: $p"
    done
  fi
fi
if [ ! -f build-dxvk/build.ninja ]; then
  meson setup build-dxvk third_party/dxvk --buildtype=release \
    --prefix="$root/build-dxvk/install" \
    -Dnative_sdl3=enabled -Dnative_sdl2=disabled -Dnative_glfw=disabled \
    -Denable_d3d8=false -Denable_d3d9=false -Denable_d3d10=false
fi
ninja -C build-dxvk install
echo "dxvk-native installed to $root/build-dxvk/install"
