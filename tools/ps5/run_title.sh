#!/usr/bin/env bash
# Starts the title on the console with arguments: they go to
# PS5_TITLE_REMOTE/launch-args.txt (one per line), then oh2launch starts it.
#   tools/ps5/run_title.sh [ARG...]
# With no ARGs the runtime defaults: --game-dir $PS5_DUMP_REMOTE and a log
# file in the title's logs folder (pull it with tools/ps5/pull_logs.sh).
set -euo pipefail
source "$(dirname "$0")/lib.sh"
ps5_need_console
args=("$@")
if [[ ${#args[@]} -eq 0 ]]; then
  args=(--game-dir "$PS5_DUMP_REMOTE"
        --env "FL_LOG_FILE=$PS5_LOG_REMOTE/runtime.txt"
        --env "FL_LOG=${FL_LOG:-loader}"
        --env "XDG_DATA_HOME=$PS5_TITLE_REMOTE/data")
fi
tmp=$(mktemp)
printf '%s\n' "${args[@]}" > "$tmp"
ps5ftp put "$tmp" "$PS5_TITLE_REMOTE/launch-args.txt" > /dev/null
rm -f "$tmp"
"$ps5_root/tools/ps5/send_payload.sh" "$ps5_root/build-ps5/launch/oh2launch.elf"
