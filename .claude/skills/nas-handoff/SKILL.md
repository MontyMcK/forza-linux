---
name: nas-handoff
description: Use when Open Horizon 2 is set up on a new machine or laptop, when you start on this project with no context, or when asked about the project's NAS folder, backups or handoff. Explains the "Open Horizon 2" folder on Monty's NAS (offline repo bundle, private research, agent notes, console settings, START HERE guide), how to restore a working setup from it, and how tools/nas_sync.sh keeps it current on every push.
---

# The project's NAS handoff folder

Monty wants an agent on another machine (Monty's laptop) with no context to be able to build as well as the one that wrote this. Everything that isn't in git lives in **`Open Horizon 2`** on Monty's NAS (`//192.168.1.2/Montys Network Storage`, mounted at `~/NetworkStorage` by `~/mount-network-storage.sh` on Monty's PC).

## Starting on a new machine

1. Open `START HERE.md` in that folder (the same text as `docs/NEW_MACHINE.md`, plus when it was last synced and the branch heads).
2. Clone from GitHub, or from `repo/open-horizon-2.bundle` when offline.
3. Restore: `research-private/` -> `research/private/`, `config/ps5.env` -> `tools/ps5/ps5.env` (fix PS5_LOG_HOST for the new machine), and read `agent-notes/` (Monty's preferences, machine and console gotchas).
4. Follow docs/NEW_MACHINE.md for packages and builds, then CLAUDE.md's reading order.

The game dump is **not** on the NAS (Monty supplies it; never copy it there unless Monty asks).

## Keeping it current

- `tools/git-hooks/pre-push` runs `tools/nas_sync.sh --background` on every push (hooks are active with `git config core.hooksPath tools/git-hooks`). It skips quietly if the NAS isn't mounted and never blocks a push. Log: `research/private/nas-sync.log` in the main checkout.
- Run it by hand after changing anything that isn't in git (research/private, ps5.env, agent notes): `tools/nas_sync.sh` (foreground) or `--background`.
- What it copies: a git bundle of every branch; the main checkout's `research/private/` (mirrored) plus a worktree's PS5 logs; the Claude memory notes about this project (forza-*, anyps5-*, console, Monty's setup and rules); `tools/ps5/ps5.env`; and `START HERE.md`.
- When you add something a fresh agent would need that isn't in git, add it to `tools/nas_sync.sh` and to the table in docs/NEW_MACHINE.md.
