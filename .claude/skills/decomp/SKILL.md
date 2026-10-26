---
name: decomp
description: Use whenever you are stuck on the Forza Horizon 2 Linux runtime (a crash, hang, unhandled exception, unknown interface or vtable slot, a WinRT/API answer you are unsure of, or game behaviour you can't explain). Look it up in the decompilations (the game itself, the Azimuth launcher, the Microsoft reference DLLs) before guessing or experimenting.
---

# When stuck, read the decomp

Monty's standing instruction: **whenever you get stuck, refer to the decomp.** Don't guess or run blind experiments while a decompiler can answer the question. Three sources, in this order:

1. **The game** (`forza_durango_release_final.exe`): what the game expects. Ghidra project `research/private/game_ghidra/fh2`.
2. **Azimuth** (a proprietary Windows launcher that runs this game): how a working runtime answers the same call. Ghidra project `research/private/azimuth/ghidra/azimuth` (program `azimuth.exe`). Its strings are already extracted to `research/private/azimuth/strings_a.txt` / `strings_w.txt`, and its D3D11.X slot-name table to `iface_slot_names.txt`. Its messages are descriptive ("offline secure template SessionRequirement", "Azimuth.OfflineTokenRequest"), so **grep the strings first**.
3. **Microsoft reference DLLs** in `game_data/.../EmbeddedXvd/Windows/System32/` (umd.dll, kernelx.dll, msvcr110.dll…): how the real API behaves. Use `python3 -P tools/pe_re.py` and `llvm-objdump`.

## Rules (CLAUDE.md Rule 0/1)

- Learn **behaviour** (answers, layouts, error codes, slot numbers), then implement it ourselves. Never copy code, shaders or tables from Azimuth or the Microsoft DLLs, and never load, link or ship them.
- All decompiler output goes in `research/private/` (gitignored). Tracked files get only short facts plus evidence (function RVA, string), written in `docs/FINDINGS.md`.

## 1. Get the addresses

- Crash or unhandled exception: the report prints game backtraces (`forza_durango_release_final.exe+0x…`).
- Hang: `FL_DUMP_THREADS_AT=70,100` logs every thread's native frames and game RVAs at those seconds (channel `threads`). Threads whose RIP is in game code are busy, while threads in `fl::kx::wait_for` are blocked; compare the two dumps.
- Exceptions: `FL_LOG=seh` logs every C++ throw/rethrow, the catch that took it, and chores caught at a native boundary.
- Would anything catch it? `python3 -P tools/eh_info.py <exe or dll> <rva>...` prints the handler, the try state at that RVA and each try block's catches.

## 2. Decompile (headless Ghidra)

```bash
export JAVA_HOME=~/.local/share/jdks/jdk-25.0.4.1+1 GHIDRA_HEADLESS_MAXMEM=24G
H=~/tools/ghidra_12.2_PUBLIC/support/analyzeHeadless
# the game
$H research/private/game_ghidra fh2 -process forza_durango_release_final.exe -noanalysis -readOnly \
   -scriptPath tools/ghidra -postScript DecompileRefs.java "$PWD/research/private/game_decomp/<topic>" \
   addr:0x1839500 callers:0x183bed0
# Azimuth
$H research/private/azimuth/ghidra azimuth -process azimuth.exe -noanalysis -readOnly \
   -scriptPath tools/ghidra -postScript DecompileRefs.java "$PWD/research/private/azimuth/decomp/<topic>" \
   "str:<text from strings_a.txt>" vt:LocalUser
```

Queries: `addr:<rva>` (the function containing it), `callers:<rva>` (code and data references), `str:<text>` (functions using an ASCII/UTF-16 string), `fn:<name>`, `vt:<class>` (every slot of an MSVC `Class::vftable`, which Azimuth has RTTI for). The output is one `.c` file per function plus `index.txt`. Pass the output dir as an absolute path. A run takes 1–3 min. Read earlier results before re-running: `ls research/private/game_decomp research/private/azimuth/decomp`.

## 3. Typical loop

1. From the backtrace, thread dump or log, find the game function and decompile it plus its callers until you see the **condition** that leads to the bad path. Example: the session poller expects the job to return an error code and retries after 30 s.
2. Name the WinRT class/method or API involved (our logs print the class and the slot).
3. `grep -n -i '<class or method>' research/private/azimuth/strings_*.txt`, decompile the referencing functions (`str:`), and for Azimuth classes `vt:<Name>` to see each slot's answer.
4. Diff Azimuth's answer against ours in `src/hle/...`. Look at return values, out-params, HRESULTs and whether an async op completes, fails or stays pending.
5. Implement it, run, and record the fact plus evidence in `docs/FINDINGS.md` (Rule 2).

If Azimuth answers the same way we do and the game still breaks, the difference is in our runtime (threading, PPL/ConcRT, SEH/C++ EH, D3D). Read the reference msvcr110/umd for the real semantics.

## Test runs (Monty's defaults)

- Skip both long movies: `FL_SKIP_MOVIES=Anthem_Tone_Video,IE_opening` (needs a `-DFL_DEV_SKIP_MOVIES=ON` build, like `build/`; the CMake default must stay OFF in anything pushed).
- Run test builds under MangoHud: `mangohud ./build/forza-linux ...`. Use the overlay only: MangoHud's own file logging (`log_duration`) crashed the game when the log ended.
- Use a separate save, `XDG_DATA_HOME=<scratch>/xdg`, and never start a test while Monty's own game is running (`pgrep -a forza-linux`): both share the GPU.
- Standard run: `XDG_DATA_HOME=<scratch>/xdg FL_SKIP_MOVIES=Anthem_Tone_Video,IE_opening FL_AUTOPRESS="40:A,42:A,46:A,50:A,55:A,60:A" mangohud ./build/forza-linux --game-dir <Mount>`. Gameplay starts at ~100 s.
