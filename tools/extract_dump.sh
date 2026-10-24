#!/usr/bin/env bash
# Extracts the user's dump into game_data/ (gitignored).
# Usage: tools/extract_dump.sh [path/to/dump.rar] [file-inside-archive ...]
#   No file args = extract everything (~40 GB, needs the disk space).
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
archive="${1:-$root/forza 2.rar}"; shift || true
mkdir -p "$root/game_data"
unar -q -f -o "$root/game_data" -D "$archive" "$@"
echo "Extracted to $root/game_data"
