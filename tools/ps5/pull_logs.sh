#!/usr/bin/env bash
# Pulls the console's logs into research/private/ps5-logs/<time>/ (gitignored)
# and prints where they went.
#   tools/ps5/pull_logs.sh [REMOTE_DIR...]    default: PS5_LOG_REMOTE
set -euo pipefail
source "$(dirname "$0")/lib.sh"
ps5_need_console
out="$ps5_root/research/private/ps5-logs/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"
dirs=("$@")
[[ ${#dirs[@]} -gt 0 ]] || dirs=("$PS5_LOG_REMOTE")
for d in "${dirs[@]}"; do
  ps5ftp get "$d" "$out/$(basename "$d")"
done
ln -sfn "$out" "$ps5_root/research/private/ps5-logs/latest"
echo "$out"
