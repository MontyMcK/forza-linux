#!/usr/bin/env bash
# Builds the title folder build-ps5/title/<TITLE_ID>/ from an eboot.bin, and
# with --zip the release zip build-ps5/release/OpenHorizon2-<TITLE_ID>.zip.
# The staged folder is audited first (tools/check_no_game_data.sh --package);
# if the audit fails, nothing is built.
#   tools/ps5/package.sh EBOOT_BIN [--zip]
set -euo pipefail
source "$(dirname "$0")/lib.sh"
eboot=${1:?usage: package.sh EBOOT_BIN [--zip]}
zip=${2:-}
title="$ps5_root/build-ps5/title/$PS5_TITLE_ID"
stage="$ps5_root/build-ps5/title/.stage-$PS5_TITLE_ID"
libc="$ps5_root/build-ps5/deps/PS5_Vulkan/runtime/libc.prx"
(cd "$(dirname "$libc")" && sha256sum --check --strict --quiet libc.prx.sha256)
rm -rf "$stage"
mkdir -p "$stage/sce_sys" "$stage/sce_module"
cp "$eboot" "$stage/eboot.bin"
cp "$ps5_root/ps5/title/sce_sys/param.json" "$ps5_root/ps5/title/sce_sys/icon0.png" "$stage/sce_sys/"
cp "$libc" "$stage/sce_module/libc.prx"
if ! "$ps5_root/tools/check_no_game_data.sh" --package "$stage"; then
  rm -rf "$stage"
  exit 1
fi
rm -rf "$title"
mv "$stage" "$title"
echo "title folder: $title"
if [[ $zip == --zip ]]; then
  rel="$ps5_root/build-ps5/release"
  mkdir -p "$rel"
  rm -f "$rel/OpenHorizon2-$PS5_TITLE_ID.zip"
  (cd "$(dirname "$title")" && zip -qr "$rel/OpenHorizon2-$PS5_TITLE_ID.zip" "$PS5_TITLE_ID")
  echo "release zip: $rel/OpenHorizon2-$PS5_TITLE_ID.zip"
fi
