# Open Horizon 2 on a new machine (start here)

For an agent or a person picking the project up with no context, e.g. on Monty's laptop. Everything not in git lives in the project's folder on Monty's NAS (`Open Horizon 2`, kept current by `tools/nas_sync.sh` on every push; the skill `.claude/skills/nas-handoff` explains it). This file is that folder's `START HERE.md`.

## 1. Read, in this order

1. `CLAUDE.md` (the rules: game data never in git, clean room, keep the trail, fail loudly)
2. `docs/PLAN.md` and, for the PS5 port, `docs/PS5_PORT.md` (checklist; resume at the first unchecked box)
3. `docs/FINDINGS.md` (facts already established; section 7 is the PS5)
4. The last 5 entries of `docs/LOG.md` (exact next step)
5. NAS `agent-notes/` (what the previous agent learned about Monty, Monty's machine and console: see 4 below)

## 2. Get the code

- GitHub: `git clone https://github.com/MontyMcK/open-horizon-2` (branches: `main`, `test` = what Monty plays; `ps5-test` = PS5 work, `ps5` = PS5 builds Monty passed).
- Offline: `git clone "<NAS>/Open Horizon 2/repo/open-horizon-2.bundle" open-horizon-2` then `git remote set-url origin https://github.com/MontyMcK/open-horizon-2`.
- Then: `git config core.hooksPath tools/git-hooks` (game-data check before every commit, NAS sync after every push), and the repo-local identity `MontyMcK <22246371+MontyMcK@users.noreply.github.com>`.

## 3. Restore what isn't in git (from the NAS folder)

| NAS | Put it at | What it is |
| --- | --- | --- |
| `research-private/` | `research/private/` | Ghidra projects (game, Azimuth), decompiles, logs, screenshots, PS5 console logs. Game-derived: never commit it |
| `config/ps5.env` | `tools/ps5/ps5.env` | console IP, ports, title ID, dump path, log host (edit PS5_LOG_HOST to the new machine's IP) |
| `agent-notes/*.md` | the agent's memory, or just read them | Monty's preferences and hard-won gotchas |

**Not on the NAS:** the game dump. Monty supplies it: ask Monty. On a PC it is extracted into `game_data/` (`tools/extract_dump.sh`); on the PS5 it is at `/data/OpenHorizon2/Anthem_1.0.0.64_x64__8wekyb3d8bbwe/Mount`.

## 4. Machine setup (Fedora; other Linux: the same packages)

```bash
sudo dnf install clang llvm lld cmake ninja-build meson glslang SDL3-devel vulkan-headers \
  vulkan-loader-devel zlib-devel python3-pip python3-pyside6 unar p7zip curl rsync ImageMagick open-sans-fonts \
  spirv-llvm-translator-devel libclc-devel spirv-headers-devel python3-ply clang-devel   # last line: Mesa (PS5 RADV)
pip install --user pefile capstone
```

- Linux build: `tools/build_dxvk.sh`, `tools/build_ffmpeg_xma.sh`, then `cmake -S . -B build -G Ninja && ninja -C build`. Check: `./build/os_test`, `./build/gcn_layout_test`, `./build/dxvk_smoke`, and a run with the dump: `FL_EXIT_AFTER_FRAME=300 mangohud ./build/forza-linux --game-dir <Mount>` must exit cleanly.
- Launcher: `python3 launcher/forza_launcher.py` (README).
- PS5: the payload SDK release v0.44 unzipped at `~/ps5-payload-sdk` (https://github.com/ps5-payload-dev/sdk/releases/tag/v0.44), then `tools/ps5/setup_deps.sh` (pinned PS5_Vulkan/PS5_PayloadSDK/PS5_Mesa into `build-ps5/deps`). Mesa's generators need mako/markupsafe: `python3 -m pip install --target build-ps5/deps/pymods mako markupsafe` and `PYTHONPATH=build-ps5/deps/pymods`. Scripts: `tools/ps5/` (table in docs/PS5_PORT.md).
- Reverse engineering: Ghidra 12.2 (`~/tools/ghidra_12.2_PUBLIC`) with JDK 25 (`JAVA_HOME=~/.local/share/jdks/jdk-25.0.4.1+1`); the `decomp` skill has the workflow.

## 5. Working with Monty (from agent-notes)

- Never credit Claude in commits or PRs (no Co-Authored-By, no "Generated with").
- Linux test runs: `mangohud ./build/forza-linux ...`; developer builds skip the long movies with `FL_SKIP_MOVIES=Anthem_Tone_Video,IE_opening` (needs `-DFL_DEV_SKIP_MOVIES=ON`, which must never be ON in anything pushed).
- Don't start a test instance while Monty plays their own (it tanks their frame rate); Monty watches test windows, so say first when a run is a deliberate old/unfixed build.
- Ask before sudo, deleting anything, uploading the dump, or new-licence dependencies. Short, plain replies.
- PS5: Monty runs the jailbreak (and elfldr) after each restart; all tests that need a person go to Monty as a TEST CARD. Never play, watch or judge gameplay yourself.
- The PS5 title is `PPSA99642` in `/data/homebrew/PPSA99642` (ShadowMount+, **1.7beta4 or newer**: older versions don't mount `/data` and USB into titles, and the title then can't see the dump). Redeploy only with `tools/ps5/upload_title.sh` (atomic: a missing eboot.bin makes ShadowMount+ drop the title until a console restart).
