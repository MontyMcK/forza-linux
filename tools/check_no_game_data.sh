#!/usr/bin/env bash
# Fails if any tracked (or, with --staged, staged) file looks like game data.
# Run it before every commit. The pre-commit hook runs it automatically.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

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
