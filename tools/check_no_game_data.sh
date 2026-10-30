#!/usr/bin/env bash
# Fails if any tracked (or, with --staged, staged) file looks like game data.
# Run it before every commit. The pre-commit hook runs it automatically.
#
# --package DIR audits a build output (the PS5 title folder, a release zip's
# contents) instead: see package_audit below. tools/ps5/package.sh and
# tools/ps5/upload_title.sh run it and stop if it fails.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

# Every file in DIR is checked against:
#  1. the local dump: same file name and size as any file in game_data/, or the
#     same bytes as a dump file of the same size (a renamed copy). The listing
#     is made now, in memory, and never written anywhere. Without a dump on this
#     machine the audit fails, unless OH2_AUDIT_NO_DUMP=1 says to skip check 1.
#  2. the dump's file types and the names CLAUDE.md Rule 0 lists (appdata.bin,
#     Catalog*.bin, hash.manifest, anything under EmbeddedXvd/ or Mount/).
#  3. a size limit (OH2_AUDIT_MAX_MB, default 200 MiB) and binary magics
#     (PE, DXBC, RAR, zip, Bink, FMOD bank).
package_audit() {
  local dir=$1 bad=0 listing
  [[ -d $dir ]] || { echo "check_no_game_data: $dir is not a folder"; exit 2; }
  local max_bytes=$(( ${OH2_AUDIT_MAX_MB:-200} * 1024 * 1024 ))
  # The dump: this checkout's game_data/, or the main checkout's when this is a worktree.
  local dump=${OH2_GAME_DATA:-}
  if [[ -z $dump ]]; then
    for d in game_data "$(git rev-parse --path-format=absolute --git-common-dir)/../game_data"; do
      [[ -d $d ]] && { dump=$d; break; }
    done
  fi
  if [[ -n $dump && ! -d $dump ]]; then
    echo "check_no_game_data: OH2_GAME_DATA=$dump is not a folder"; exit 2
  elif [[ -n $dump ]]; then
    listing=$(find -L "$dump" -type f -printf '%s\t%f\t%p\n')
    [[ -n $listing ]] || { echo "check_no_game_data: $dump is empty"; exit 2; }
  elif [[ ${OH2_AUDIT_NO_DUMP:-} == 1 ]]; then
    echo "check_no_game_data: no game_data/ here; OH2_AUDIT_NO_DUMP=1, skipping the name/size check"
    listing=""
  else
    echo "check_no_game_data: no game_data/ to compare against (set OH2_GAME_DATA, or OH2_AUDIT_NO_DUMP=1)"
    exit 2
  fi
  local n=0 f rel name size magic
  while IFS= read -r -d '' f; do
    n=$((n + 1))
    rel=${f#"$dir"/}
    name=${f##*/}
    size=$(stat -c %s "$f")
    case "/${rel,,}/" in
      */embeddedxvd/*|*/mount/*)
        echo "BLOCKED (dump folder name): $rel"; bad=1; continue ;;
    esac
    case "${name,,}" in
      appdata.bin|catalog*.bin|hash.manifest|*.xvd|*.xvc|*.msixvc|*.rar|*.7z|*.exe|*.dll|*.sys|*.pso|*.vso|*.cso|*.dxbc|*.shaderbin|*.bank|*.bk2|*.xma|*.zip|*.minizip|*.nt|*.owt|*.tz|*.avpins|*.lightdb|*.lightrtudb|*.plib|*.swatchbin|*.fxb|*.track|*.pvs|*.particlebin|*.materialbin|*.lights|*.imap|*.col|*.regtrans-ms|*.vbi)
        echo "BLOCKED (game file name/type): $rel"; bad=1; continue ;;
    esac
    if (( size > max_bytes )); then
      echo "BLOCKED (over ${OH2_AUDIT_MAX_MB:-200} MiB): $rel ($size bytes)"; bad=1; continue
    fi
    magic=$(head -c 4 "$f" | od -An -c | tr -d ' ')
    case "$magic" in
      MZ*|DXBC|Rar!*|PK*|KB2*|BIK*|RIFF)
        if [[ $magic != RIFF ]] || head -c 12 "$f" | tail -c 4 | grep -q 'FEV '; then
          echo "BLOCKED (binary magic '$magic'): $rel"; bad=1; continue
        fi ;;
    esac
    if [[ -n $listing ]]; then
      local same hit=""
      same=$(awk -F'\t' -v s="$size" '$1 == s' <<<"$listing")
      if [[ -n $same ]]; then
        if awk -F'\t' -v n="${name,,}" 'tolower($2) == n { found = 1 } END { exit !found }' <<<"$same"; then
          hit="same name and size"
        else
          while IFS=$'\t' read -r _ _ p; do
            cmp -s "$f" "$p" && { hit="same bytes as a dump file"; break; }
          done <<<"$same"
        fi
      fi
      if [[ -n $hit ]]; then
        echo "BLOCKED ($hit): $rel ($size bytes)"; bad=1; continue
      fi
    fi
  done < <(find "$dir" -type f -print0)
  if (( bad )); then
    echo
    echo "Package refused: it holds something from the game dump (CLAUDE.md, Rule 0)."
    exit 1
  fi
  echo "package audit: $n files in $dir, nothing from the dump"
  exit 0
}

if [[ "${1:-}" == "--package" ]]; then
  package_audit "$(realpath "${2:?usage: check_no_game_data.sh --package DIR}")"
fi

if [[ "${1:-}" == "--staged" ]]; then
  mapfile -t files < <(git diff --cached --name-only --diff-filter=ACMR)
else
  mapfile -t files < <(git ls-files)
fi

bad=0
max_bytes=$((2 * 1024 * 1024))   # nothing we write by hand is >2 MB
for f in "${files[@]}"; do
  [[ -f "$f" ]] || continue
  case "$f" in
    game_data/*|research/private/*|runtime/*|screenshots/*)
      echo "BLOCKED (private dir): $f"; bad=1; continue ;;
  esac
  case "${f,,}" in
    *.rar|*.7z|*.xvd|*.xvc|*.exe|*.dll|*.sys|*.pso|*.vso|*.cso|*.dxbc|*.shaderbin|*.bank|*.bk2|*.xma|*.zip)
      echo "BLOCKED (game file type): $f"; bad=1; continue ;;
  esac
  size=$(stat -c %s "$f")
  if (( size > max_bytes )); then
    echo "BLOCKED (>2MB, probably not hand-written): $f"; bad=1; continue
  fi
  magic=$(head -c 4 "$f" | od -An -c | tr -d ' ')
  case "$magic" in
    MZ*|DXBC|Rar!*|burG|PK*)
      echo "BLOCKED (binary magic '$magic'): $f"; bad=1 ;;
  esac
done

if (( bad )); then
  echo
  echo "Commit refused: game data must never enter git (CLAUDE.md, Rule 0)."
  exit 1
fi
