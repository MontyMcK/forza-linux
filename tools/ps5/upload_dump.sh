#!/usr/bin/env bash
# Uploads YOUR OWN dump's Mount folder to PS5_DUMP_REMOTE over FTP (~40 GB, hours on Wi-Fi).
# Never run it unless the console's owner asked for it in this session:
#   tools/ps5/upload_dump.sh --i-was-asked /path/to/Mount
set -euo pipefail
source "$(dirname "$0")/lib.sh"
[[ ${1:-} == --i-was-asked ]] || { echo "refusing: this uploads the game dump; pass --i-was-asked only when told to" >&2; exit 2; }
src=${2:?usage: upload_dump.sh --i-was-asked /path/to/Mount}
[[ -f $src/forza_durango_release_final.exe ]] || { echo "$src is not a Mount folder" >&2; exit 2; }
ps5_need_console
ps5ftp put --dump "$src" "$PS5_DUMP_REMOTE"
