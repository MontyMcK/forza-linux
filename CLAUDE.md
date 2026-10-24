# CLAUDE.md — read this first, every session

Project: run the Xbox One build of **Forza Horizon 2** natively on **Linux**.
We are not porting Azimuth Launcher (a proprietary Windows tool by Almost There Games, all rights reserved). This is a clean-room Linux implementation. A binary build of Azimuth is available for study (Rule 1).

Start every session by reading, in this order:
1. This file
2. [docs/PLAN.md](docs/PLAN.md): the roadmap and the current phase. Find the first unchecked task.
3. [docs/FINDINGS.md](docs/FINDINGS.md): facts already established about the binary. Don't re-derive them.
4. The last ~5 entries of [docs/LOG.md](docs/LOG.md): what the previous agent was doing and where it got stuck.

---

## Rule 0: game data is secret and never goes in git

The game dump is not ours to distribute. **No game data may ever be committed.**

- The dump lives in `forza 2.rar` (project root) and is extracted into `game_data/`. Both are gitignored.
- Anything derived from the binary also stays out of git: disassembly listings, decompiler output, Ghidra/IDA projects, extracted shaders, asset dumps, raw logs, screenshots of the game. Put it in `research/private/` (gitignored).
- Hand-written facts are fine in git: API names, struct layouts, vtable slot numbers, RVAs of functions, short descriptions of behavior. Write those in `docs/FINDINGS.md`. Don't paste multi-line disassembly or decompiled code into tracked files.
- Never commit hashes, license blobs, `appdata.bin`, `Catalog*.bin`, `hash.manifest` or anything else from `EmbeddedXvd/` or `Mount/`. Some of these can be tied to the console or account the dump came from.
- `tools/check_no_game_data.sh` runs as a pre-commit hook (`git config core.hooksPath tools/git-hooks`). Never bypass it with `--no-verify`. If it blocks something, the file shouldn't be committed.
- The runtime must take the game directory as a path argument (`--game-dir`). No game paths are hard-coded to this machine, and no game files ship with the project.

## Rule 1: clean room

- Don't use leaked Xbox XDK headers, docs or samples. Recover API layouts by reverse engineering how the game calls them, and write down the evidence in FINDINGS.md.
- The Microsoft DLLs in `game_data/.../EmbeddedXvd/Windows/System32/` (kernelx.dll, d3d11_x.dll, combase.dll, msvcr110.dll…) may be **read and disassembled as reference** to learn behavior. They must **never be loaded, linked or shipped** by our runtime.
- Azimuth (`azimuth-windows.zip`, unpacked in `research/private/azimuth/`) is proprietary: treat it exactly like the Microsoft DLLs. Read, disassemble and decompile it to learn **how** it handles the game (protocols, layouts, semantics), write the facts in FINDINGS.md with the Azimuth function/string as evidence, and implement them ourselves. Never copy its code, shaders or pipeline cache, and never load, link or ship it. Decompiler output stays in `research/private/`.
- Wine is LGPL-2.1+. Reading it for reference is fine. If you copy code from it, keep that code in its own files under its original license header, and note it in LOG.md.

## Rule 2: keep the trail for the next agent

Other agents (often weaker models) continue this work. At the end of every working session:
- Tick off finished tasks in docs/PLAN.md, and add any new subtasks you discovered.
- Add every confirmed fact to docs/FINDINGS.md, with evidence (RVA, call site, how you verified it).
- Append a dated entry to docs/LOG.md: what you did, what works, what's broken, the exact next step.
- If you made a design decision, write it in the "Decisions" section of PLAN.md with a one-line reason.

## Rule 3: fail loudly

- Every unimplemented import or COM/WinRT method must log `UNIMPLEMENTED <dll>!<name>` (or `<Interface>::<method> slot N`) and then abort or return an error. Don't silently return 0. Silent stubs cost days of debugging.
- A stub that does return a fake success must log `STUB` once and carry a `// STUB:` comment explaining why.

## Environment (the user's machine)

- Fedora 44, kernel 7.x, x86-64, 24 threads, 62 GB RAM
- GPU: AMD Radeon RX 9070 XT (RDNA4, RADV GFX1201) → Mesa **RADV** Vulkan
- Tools already installed: clang/llvm (`llvm-objdump`, `llvm-readobj`), `unar`/`lsar`, 7z, python3 with `pefile` + `capstone` (pip --user), meson, glslang, SDL3-devel, vulkan-headers, vulkan-loader-devel, ffmpeg-free-devel. Not installed yet: Ghidra. Install what you need with `dnf` or `pip --user` (ask the user before using sudo).

## Handy commands

```bash
tools/extract_dump.sh                                    # full extract to game_data/ (~40 GB)
tools/extract_dump.sh "forza 2.rar" "<path in archive>"  # extract a single file
lsar -l "forza 2.rar"                                    # list archive contents
GAME=game_data/Anthem_1.0.0.64_x64__8wekyb3d8bbwe/Mount
llvm-readobj --coff-imports $GAME/forza_durango_release_final.exe
tools/check_no_game_data.sh                              # verify the repo is clean
tools/build_dxvk.sh                                      # build dxvk-native (needed before cmake)
cmake -S . -B build -G Ninja && ninja -C build           # build forza-linux + dxvk_smoke
./build/dxvk_smoke                                       # check DXVK works on this machine
FL_LOG=d3d ./build/forza-linux --game-dir $GAME          # run the game (logs: FL_LOG=all)
python3 -P tools/pe_re.py slots $SYS32/umd.dll 0x16f2e0 0-83   # Xbox D3D vtable slots (FINDINGS §3.1)
```
(`SYS32=game_data/Anthem_1.0.0.64_x64__8wekyb3d8bbwe/EmbeddedXvd/Windows/System32`)

## Code style

- C++20, built with **clang** (needed for `__attribute__((ms_abi))`) and CMake + Ninja.
- Every function the game calls directly, and every vtable method we hand to the game, is declared `__attribute__((ms_abi))`. Use the `WINAPI` macro from `src/common/winabi.h`.
- Windows types are not Linux types: `long`/`LONG` is **32-bit**, `wchar_t` on Windows is **16-bit** (use `char16_t`), `BOOL` is int. Never use Linux `wchar_t` or `long` in an ABI boundary struct.
- Add a small test in `tests/` for anything that can be tested without the game (string functions, the unwinder, wait objects…).
