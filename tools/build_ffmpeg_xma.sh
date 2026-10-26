#!/usr/bin/env bash
# Builds a minimal FFmpeg libavcodec with only the XMA1/XMA2 decoders (LGPL,
# libavcodec/wmaprodec.c, plus our patches in tools/ffmpeg-xma-patches) into
# build-ffmpeg-xma/install (gitignored). The
# runtime uses it to decode the XMA audio the game sends to the SHAPE audio
# hardware (src/hle/acphal/shape.cpp); Fedora's ffmpeg-free has no XMA decoders.
# Usage: tools/build_ffmpeg_xma.sh
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
ver=7.1.1
sha256=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1  # ffmpeg-7.1.1.tar.xz (signature checked)
out="$root/build-ffmpeg-xma"
mkdir -p "$out/dl"
tarball="$out/dl/ffmpeg-$ver.tar.xz"
if [ ! -f "$tarball" ]; then
  curl -fL -o "$tarball.part" "https://ffmpeg.org/releases/ffmpeg-$ver.tar.xz"
  mv "$tarball.part" "$tarball"
fi
echo "$sha256  $tarball" | sha256sum -c -
if [ ! -d "$out/src" ]; then
  mkdir -p "$out/src"
  tar -xf "$tarball" -C "$out/src" --strip-components=1
fi
cd "$out/src"
# Our changes (tools/ffmpeg-xma-patches/*.patch), applied once each.
for p in "$root"/tools/ffmpeg-xma-patches/*.patch; do
  if patch -p1 -N --dry-run --silent < "$p" > /dev/null 2>&1; then
    patch -p1 -N --silent < "$p"
    echo "applied $(basename "$p")"
  fi
done
if [ ! -f "$out/src/ffbuild/config.mak" ]; then
  ./configure --prefix="$out/install" \
    --disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect \
    --disable-avdevice --disable-avformat --disable-swscale --disable-swresample --disable-avfilter \
    --enable-shared --disable-static --enable-pic --disable-x86asm \
    --enable-decoder=xma1,xma2 --build-suffix=_flxma
fi
make -j"$(nproc)"
make install
echo "FFmpeg XMA decoders installed to $out/install"
