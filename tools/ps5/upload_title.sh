#!/usr/bin/env bash
# Uploads the packaged title folder to PS5_TITLE_REMOTE after the Rule 0 audit.
#   tools/ps5/upload_title.sh [TITLE_DIR]     default: build-ps5/title/$PS5_TITLE_ID
# Close the title on the console first: a running title's files can't be replaced.
set -euo pipefail
source "$(dirname "$0")/lib.sh"
dir=${1:-$ps5_root/build-ps5/title/$PS5_TITLE_ID}
[[ -d $dir ]] || { echo "no title folder at $dir (run tools/ps5/package.sh)" >&2; exit 2; }
"$ps5_root/tools/check_no_game_data.sh" --package "$dir"
ps5_need_console
ps5ftp put "$dir" "$PS5_TITLE_REMOTE"
