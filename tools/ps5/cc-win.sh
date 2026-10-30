#!/usr/bin/env bash
# The PS5 compiler for code that uses the Windows x64 calling convention (the
# runtime: every HLE export is __attribute__((ms_abi))). clang's x86_64-sie-ps5
# target rejects ms_abi, so this compiles for FreeBSD x86-64 (the PS5's ABI)
# against the PS5 SDK, with the PS5 target's differences put back (FINDINGS 7.6).
# -nostdinc: for this target clang would also search the host's /usr/include
# (libc++ then took glibc's mbstate_t).
# Objects link into a title with tools/ps5/link_title.sh like the others.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
sdk="$root/build-ps5/deps/PS5_Vulkan/.deps/native/ps5-payload-sdk"
[[ -x $sdk/bin/prospero-lld ]] || { echo "run tools/ps5/setup_deps.sh first" >&2; exit 2; }
exec clang -target x86_64-unknown-freebsd9 -march=znver2 -fPIC \
  -fshort-wchar -D__PROSPERO__=1 -D__SCE__=1 \
  -isysroot "$sdk" -nostdinc -nostdinc++ -isystem "$sdk/target/include/c++/v1" \
  -isystem "$(clang -print-resource-dir)/include" -isystem "$sdk/target/include" \
  -fno-stack-protector -fno-plt -femulated-tls -fdenormal-fp-math=ieee \
  -Wno-unused-command-line-argument "$@"
