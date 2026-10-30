#!/usr/bin/env bash
# Keeps the project's handoff folder on Monty's NAS current: everything a new
# machine (or an agent with no context) needs that isn't in git. See
# docs/NEW_MACHINE.md and .claude/skills/nas-handoff/SKILL.md.
# Runs after every push (tools/git-hooks/pre-push, in the background) and by hand:
#   tools/nas_sync.sh [--background]
# OH2_NAS_DIR overrides the folder (default ~/NetworkStorage/Open Horizon 2).
# The game dump is never copied.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
main="$(cd "$(git -C "$root" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)"  # the main checkout
nas="${OH2_NAS_DIR:-$HOME/NetworkStorage/Open Horizon 2}"
log="$main/research/private/nas-sync.log"

if [[ ${1:-} == --background ]]; then
  mkdir -p "$(dirname "$log")"
  nohup "$0" > "$log" 2>&1 < /dev/null &
  echo "nas_sync: updating the NAS copy in the background (research/private/nas-sync.log)"
  exit 0
fi

# Only when the NAS share is really mounted (an empty mount point is local disk).
if [[ -z ${OH2_NAS_DIR:-} ]] && ! mountpoint -q "$HOME/NetworkStorage"; then
  echo "nas_sync: ~/NetworkStorage is not mounted (~/mount-network-storage.sh): skipped"
  exit 0
fi
mkdir -p "$main/research/private"
exec 9> "$main/research/private/.nas-sync.lock"
# A push during a sync waits for it, so the NAS always ends with the latest push.
flock -w 7200 9 || { echo "nas_sync: another sync held the lock for 2 hours: skipped"; exit 0; }

mkdir -p "$nas/repo" "$nas/research-private" "$nas/agent-notes" "$nas/config"
copy() { rsync -rt --no-perms --no-owner --no-group --modify-window=2 "$@"; }

# 1. The whole repository, every branch, as one file that git can clone from.
git -C "$root" bundle create "$nas/repo/.open-horizon-2.bundle.part" --all 2> /dev/null
mv -f "$nas/repo/.open-horizon-2.bundle.part" "$nas/repo/open-horizon-2.bundle"

# 2. research/private (game-derived: never in git): the main checkout's, plus
#    this worktree's PS5 logs when run from a worktree.
copy --delete --exclude 'nas-sync.log' --exclude '.nas-sync.lock' "$main/research/private/" "$nas/research-private/"
if [[ $root != "$main" && -d $root/research/private ]]; then
  copy "$root/research/private/" "$nas/research-private/"
fi

# 3. What the previous agent learned (Claude Code memory notes about this project).
mem="$HOME/.claude/projects/-home-monty/memory"
if [[ -d $mem ]]; then
  notes=()
  for f in "$mem"/forza-*.md "$mem"/anyps5-*.md "$mem"/console-game-backups.md "$mem"/monty-setup.md \
           "$mem"/no-claude-attribution.md "$mem"/verify-with-screenshots.md "$mem"/bash-tool-background-pid.md; do
    [[ -f $f ]] && notes+=("$f")
  done
  (( ${#notes[@]} )) && copy --delete-before "${notes[@]}" "$nas/agent-notes/"
fi

# 4. Console settings (not in git).
for env in "$root/tools/ps5/ps5.env" "$main/tools/ps5/ps5.env"; do
  [[ -f $env ]] && { cp "$env" "$nas/config/ps5.env"; break; }
done

# 5. The guide, with what was synced.
{
  cat "$root/docs/NEW_MACHINE.md"
  echo
  echo "## Last sync"
  echo
  echo "$(date '+%Y-%m-%d %H:%M %Z') from $(hostname). Branch heads in the bundle:"
  echo
  git -C "$root" for-each-ref --format='- `%(refname:short)` %(objectname:short) %(subject)' refs/heads
} > "$nas/START HERE.md"
echo "nas_sync: $nas updated ($(du -sh "$nas" 2> /dev/null | cut -f1))"
