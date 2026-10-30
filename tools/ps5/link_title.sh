#!/usr/bin/env bash
# Links PS5 objects and archives into a title eboot.bin (a fake SELF) with
# PS5_Vulkan's recipe: its app CRT, lld with its PIE linker script, the payload
# SDK's import stubs and libps5platform, then ps5-native-tool link + sign.
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
"$sdk/bin/prospero-lld" -T "$native/ps5-pie.ld" --eh-frame-hdr \
  --version-script "$native/app-symbols.map" --exclude-libs=ALL \
  -e _start -o "$work/llvm-pie.elf" "$work/app_crt.o" "$work/app_cpp_runtime.o" \
  --start-group "$@" "$sdk/target/lib/libps5platform.a" --end-group \
  --as-needed "$sdk"/target/lib/*.so
# A title doesn't get libkernel_sys, libkernel_web or libScePosixForWebKit:
# imports only they export are NULL at run time (FINDINGS 7, PS5_PORT.md traps).
null_imports=$(comm -23 \
  <("$sdk/bin/llvm-nm" -D --undefined-only "$work/llvm-pie.elf" | awk '$1 == "U" { sub(/@.*/, "", $2); print $2 }' | sort -u) \
  <(for lib in "$sdk"/target/lib/*.so; do
      case ${lib##*/} in libkernel_sys.so | libkernel_web.so | libScePosixForWebKit.so) continue ;; esac
      "$sdk/bin/llvm-nm" -D --defined-only "$lib" 2>/dev/null | awk '{ print $NF }'
    done | sort -u))
if [[ -n $null_imports ]]; then
  echo "link_title.sh: imports no title module exports (NULL at run time): ${null_imports//$'\n'/ }" >&2
  exit 1
fi
"$tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" --stub-dir "$sdk/target/lib" \
  --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf > /dev/null
"$tool" self --sign --in "$work/eboot.elf" --out "$out" --magic 0x1D3D154F > /dev/null
"$tool" self --inspect --file "$out" > /dev/null
echo "$out ($(stat -c %s "$out") bytes)"
