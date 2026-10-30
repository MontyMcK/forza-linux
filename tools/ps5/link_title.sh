#!/usr/bin/env bash
# Links PS5 objects and archives into a title eboot.bin (a fake SELF) with
# PS5_Vulkan's recipe: its app CRT, radv_link_recipe (below), the payload
# SDK's import stubs, then ps5-native-tool link + sign.
#   tools/ps5/link_title.sh OUT/eboot.bin INPUT.o|INPUT.a ...
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
out=${1:?usage: link_title.sh OUT/eboot.bin INPUT...}
shift
vulkan="$root/build-ps5/deps/PS5_Vulkan"
sdk="$vulkan/.deps/native/ps5-payload-sdk"
native="$vulkan/tooling/native"
tool="$vulkan/build/host/ps5-native-tool"
work="$root/build-ps5/link"
for f in "$sdk/bin/prospero-lld" "$tool" "$native/ps5-pie.ld" "$native/app-symbols.map"; do
  [[ -e $f ]] || { echo "link_title.sh: missing $f (run tools/ps5/setup_deps.sh)" >&2; exit 2; }
done
mkdir -p "$work" "$(dirname "$out")"
cc="$root/tools/ps5/cc.sh"
"$cc" -std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
  -c "$native/app_crt.cpp" -o "$work/app_crt.o"
"$cc" -std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
  -c "$native/app_cpp_runtime.cpp" -o "$work/app_cpp_runtime.o"
# PS5_Vulkan's link recipe (tools/radv-link.sh, as rpcs3-PS5 uses it): its
# unwind-aware linker script, libc++/libc++abi/libunwind, clang's builtins
# (emulated TLS), and the platform layer's wraps: malloc and friends go to its
# heap in direct memory (libc's stops at 13 MiB in a title), threads get 2 MiB
# stacks, and libc functions a title lacks are bound to the platform's.
# RADV_ARCHIVE names the Vulkan driver; without it an empty archive stands in.
archive=${RADV_ARCHIVE:-}
agc_stubs=()
if [[ -z $archive ]]; then
  archive="$work/no-driver.a"
  [[ -f $archive ]] || "$sdk/bin/llvm-ar" rc "$archive"
else
  # RADV calls AGC, which the SDK has no stubs for: PS5_Vulkan's link stubs
  # (the ones rpcs3-PS5's link-title.sh uses) stand in at link time.
  mkdir -p "$work/stubs"
  for pair in libSceAgc:agc_canary_link_stub.c libSceAgcDriver:agc_driver_canary_link_stub.c; do
    lib=${pair%%:*}
    "$cc" -std=c11 -O2 -fPIC -c "$vulkan/vendor/ps5/sdk/stubs/${pair#*:}" -o "$work/stubs/$lib.o"
    "$sdk/bin/prospero-lld" --shared -soname "$lib.prx" -o "$work/stubs/$lib.so" "$work/stubs/$lib.o"
    agc_stubs+=("$work/stubs/$lib.so")
  done
fi
# The recipe looks for clang's builtins at <resource-dir>/lib/linux/
# libclang_rt.builtins-x86_64.a; Fedora keeps them in lib/<triple>/
# libclang_rt.builtins.a. A shim reports a resource dir that has them there.
host_rt=$(clang -print-libgcc-file-name -rtlib=compiler-rt 2>/dev/null || true)
shim="$work/clang-shim"
if [[ -f $host_rt && ! -x $shim/clang ]]; then
  mkdir -p "$shim/rt/lib/linux"
  ln -sf "$host_rt" "$shim/rt/lib/linux/libclang_rt.builtins-x86_64.a"
  printf '#!/bin/sh\n[ "$1" = --print-resource-dir ] && { echo "%s"; exit 0; }\nexec clang "$@"\n' "$shim/rt" > "$shim/clang"
  chmod +x "$shim/clang"
fi
# shellcheck source=/dev/null
source "$vulkan/tools/radv-link.sh"
PS5_CLANG=$([[ -x $shim/clang ]] && echo "$shim/clang" || command -v clang) radv_link_recipe "$vulkan" "$sdk" "$archive" || exit 2
# A title publishes no exports.
printf '{\n    local: *;\n};\n' > "$work/title-local.map"
"$sdk/bin/prospero-lld" --error-limit=0 -z nodynamic-undefined-weak "${radv_linker_script[@]}" --eh-frame-hdr \
  "${radv_link_flags[@]}" --version-script "$work/title-local.map" \
  --version-script "$native/app-symbols.map" --exclude-libs=ALL \
  -e _start -o "$work/llvm-pie.elf" "$work/app_crt.o" "$work/app_cpp_runtime.o" \
  --start-group "$@" --end-group "${agc_stubs[@]}" "${radv_link_inputs[@]}" \
  --as-needed "$sdk"/target/lib/*.so
# A title doesn't get libkernel_sys, libkernel_web or libScePosixForWebKit:
# imports only they export are NULL at run time (FINDINGS 7, PS5_PORT.md traps).
null_imports=$(comm -23 \
  <("$sdk/bin/llvm-nm" -D --undefined-only "$work/llvm-pie.elf" | awk '$1 == "U" { sub(/@.*/, "", $2); print $2 }' | sort -u) \
  <(for lib in "$sdk"/target/lib/*.so "${agc_stubs[@]}"; do
      case ${lib##*/} in libkernel_sys.so | libkernel_web.so | libScePosixForWebKit.so) continue ;; esac
      "$sdk/bin/llvm-nm" -D --defined-only "$lib" 2>/dev/null | awk '{ print $NF }'
    done | sort -u))
if [[ -n $null_imports ]]; then
  echo "link_title.sh: imports no title module exports (NULL at run time): ${null_imports//$'\n'/ }" >&2
  exit 1
fi
stub_args=()
for s in "${agc_stubs[@]}"; do stub_args+=(--stub "$s"); done
"$tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" --stub-dir "$sdk/target/lib" "${stub_args[@]}" \
  --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf > /dev/null
"$tool" self --sign --in "$work/eboot.elf" --out "$out" --magic 0x1D3D154F > /dev/null
"$tool" self --inspect --file "$out" > /dev/null
echo "$out ($(stat -c %s "$out") bytes)"
