#!/usr/bin/env bash
# Builds DXVK for the PS5 as static libraries in build-ps5/dxvk/install (M2).
# Sources: the main checkout's third_party/dxvk working tree (DXVK v3.1.1 with
# tools/dxvk-patches applied by tools/build_dxvk.sh, plus DXVK's own bundled
# headers), copied to build-ps5/dxvk-src, then tools/ps5/dxvk-patches/*.patch
# (the PS5 Vulkan loader and display WSI). Compiler: the PS5 SDK fork's
# prospero-clang (meson cross file modelled on PS5_Vulkan's ps5-cross.ini).
set -euo pipefail
source "$(dirname "$0")/lib.sh"
main="$(cd "$(git -C "$ps5_root" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)"
upstream="$main/third_party/dxvk"
sdk="$ps5_root/build-ps5/deps/PS5_Vulkan/.deps/native/ps5-payload-sdk"
src="$ps5_root/build-ps5/dxvk-src"
build="$ps5_root/build-ps5/dxvk-build"
install="$ps5_root/build-ps5/dxvk/install"
[[ -f $upstream/meson.build ]] || { echo "no DXVK sources at $upstream (run tools/build_dxvk.sh in the main checkout)" >&2; exit 2; }
[[ -x $sdk/bin/prospero-clang ]] || { echo "run tools/ps5/setup_deps.sh first" >&2; exit 2; }

shopt -s nullglob
patches=("$ps5_root"/tools/ps5/dxvk-patches/*.patch)
shopt -u nullglob
stamp=$( (cat "${patches[@]}" /dev/null) | sha256sum | cut -c1-16)
if [[ ! -f $src/.oh2-ps5-stamp || $(<"$src/.oh2-ps5-stamp") != "$stamp" ]]; then
  rm -rf "$src" "$build"
  mkdir -p "$src"
  rsync -a --exclude .git "$upstream/" "$src/"
  for p in "${patches[@]}"; do
    [[ -f $p ]] || continue
    patch -d "$src" -p1 --forward --silent < "$p"
    echo "PS5 DXVK patch applied: $(basename "$p")"
  done
  echo "$stamp" > "$src/.oh2-ps5-stamp"
fi

cross="$ps5_root/build-ps5/dxvk-ps5.ini"
cat > "$cross" <<INI
[binaries]
c = '$sdk/bin/prospero-clang'
cpp = '$sdk/bin/prospero-clang++'
ar = '$sdk/bin/prospero-ar'
strip = '$sdk/bin/prospero-strip'
pkg-config = '$sdk/bin/prospero-pkg-config'

[host_machine]
system = 'freebsd'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'

[built-in options]
default_library = 'static'
cpp_args = ['-march=znver2', '-DDXVK_PS5=1']
c_args = ['-march=znver2', '-DDXVK_PS5=1']

[properties]
needs_exe_wrapper = true
INI
if [[ ! -f $build/build.ninja ]]; then
  meson setup "$build" "$src" --cross-file "$cross" --buildtype=release --prefix="$install" \
    -Dnative_sdl3=disabled -Dnative_sdl2=disabled -Dnative_glfw=disabled \
    -Denable_d3d8=false -Denable_d3d9=false -Denable_d3d10=false
fi
ninja -C "$build" install
echo "PS5 DXVK installed to $install"
