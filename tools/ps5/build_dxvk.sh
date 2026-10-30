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
overlay="$ps5_root/tools/ps5/dxvk-ps5"  # new files (the PS5 WSI)
stamp=$( (cat "${patches[@]}" /dev/null; find "$overlay" -type f -print0 | sort -z | xargs -0 cat) | sha256sum | cut -c1-16)
if [[ ! -f $src/.oh2-ps5-stamp || $(<"$src/.oh2-ps5-stamp") != "$stamp" ]]; then
  rm -rf "$src" "$build"
  mkdir -p "$src"
  rsync -a --exclude .git "$upstream/" "$src/"
  rsync -a "$overlay/" "$src/"
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
cpp_args = ['-march=znver2', '-DDXVK_PS5=1', '-include', '$src/include/oh2_ps5_compat.h']
c_args = ['-march=znver2', '-DDXVK_PS5=1', '-Dstatic_assert=_Static_assert', '-include', '$src/include/oh2_ps5_compat.h']

[properties]
needs_exe_wrapper = true
INI
if [[ ! -f $build/build.ninja ]]; then
  meson setup "$build" "$src" --cross-file "$cross" --buildtype=release --prefix="$install" \
    -Dnative_ps5=enabled -Dnative_sdl3=disabled -Dnative_sdl2=disabled -Dnative_glfw=disabled \
    -Denable_d3d8=false -Denable_d3d9=false -Denable_d3d10=false
fi
# DXVK's own shared d3d11/dxgi links fail here on purpose: RADV
# (vk_icdGetInstanceProcAddr) and the platform's open_memstream only come at
# the title link. Their objects become static archives instead.
ninja -C "$build" -k 0 > "$ps5_root/build-ps5/dxvk-build.log" 2>&1 || true
if grep -q 'error:' "$ps5_root/build-ps5/dxvk-build.log" && grep 'error:' "$ps5_root/build-ps5/dxvk-build.log" | grep -qv 'ld.lld\|linker command failed'; then
  grep 'error:' "$ps5_root/build-ps5/dxvk-build.log" | head -20 >&2
  echo "PS5 DXVK: compile errors (build-ps5/dxvk-build.log)" >&2
  exit 1
fi
rm -rf "$install" && mkdir -p "$install/lib"
sdkbin="$sdk/bin"
for t in d3d11 dxgi; do
  objs=("$build"/src/$t/libdxvk_$t.so.*.p/*.o)
  [[ -e ${objs[0]} ]] || { echo "PS5 DXVK: no $t objects" >&2; exit 1; }
  "$sdkbin/prospero-ar" rcs "$install/lib/libdxvk_$t.a" "${objs[@]}"
done
for a in src/dxvk/libdxvk.a src/spirv/libspirv.a src/util/libutil.a src/vulkan/libvkcommon.a src/wsi/libwsi.a \
         subprojects/dxbc-spirv/libdxbc_spv.a subprojects/libdisplay-info/libdisplay-info.a; do
  # Meson's archives are thin (they point at the objects): make real ones.
  objs=("$build/$a.p"/*.o)
  [[ -e ${objs[0]} ]] || { echo "PS5 DXVK: $a has no objects" >&2; exit 1; }
  "$sdkbin/prospero-ar" rcs "$install/lib/$(basename "$a")" "${objs[@]}"
done
echo "PS5 DXVK static libraries in $install/lib"
