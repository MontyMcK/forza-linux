# Plan: Forza Horizon 2 (Xbox One) running natively on Linux

**Current phase: Phase 6, audio (XMA streams block the intro).** With the cutscene-audio wait forced by a debug breakpoint the game fades in and reaches the festival world and its pause menu; lit geometry is still black. Previously: **Phase 5, the 3D frame.** The game loads the world after the title and renders ~900 draws a frame without crashing; with draw-bundle inheritance the 3D scene renders (visible in frame-trace dumps), but the final image is still black because the tonemap pass outputs 0 (task below). Earlier state: The game boots through the intro movies to a correct **title screen** (Bink background video, logo, A-button glyph; milestone (b)) with DualSense/keyboard input and the real FMOD engine (banks load; SHAPE/XMA voices still silent). Pressing A creates a profile in ConnectedStorage, goes through offline Live, and starts loading; the loader now drives the DMA engine context (PLAN 5b). (Update this line whenever a phase completes.)

## The big idea (read before writing any code)

The Xbox One CPU is x86-64, and so is the user's PC. The game's machine code therefore **runs directly on the host CPU**, with no instruction translation and no JIT. What is missing is everything *around* the code:

1. **A loader.** A normal Linux ELF program that maps the game's PE `.exe` into memory, applies relocations and resolves its imports.
2. **The OS it expects.** Our own implementations of the ~650 functions it imports (kernelx, the MSVC 2012 CRT, combase/WinRT, D3D11.X, audio…), compiled with the Windows x64 calling convention (`__attribute__((ms_abi))`).
3. **The Windows thread environment.** A fake TEB reached through the GS segment register, Windows-style TLS, and Windows x64 exception unwinding (SEH via `.pdata`).
4. **Graphics.** D3D11.X → Vulkan, built on **DXVK** (`dxvk-native`, which builds on Linux without Wine). The shaders are already standard DXBC.

This is how Wine works, but much smaller: one known program, a fixed set of imports, and no general Windows compatibility. The word "recomp" is loose here, since nothing needs recompiling. If a later phase ever needs it (e.g. hooking hot functions), we can patch or replace individual functions by RVA.

```
 ┌──────────────── forza-linux (ELF, our code) ────────────────┐
 │  loader: PE map + reloc + imports + TEB/GS + TLS + SEH       │
 │                                                              │
 │  game .exe (mapped at 0x140000000, runs natively)            │
 │      │ calls (ms_abi)                                        │
 │      ▼                                                       │
 │  HLE modules (ms_abi exports, by DLL name)                   │
 │   kernelx  msvcr110  msvcp110  vccorlib110  combase  ws2_32  │
 │   winrt/*  d3d11_x  xg_x  acphal  mmdevapi  mfplat  etwplus  │
 │      │                                                       │
 │      ▼ (sysv)                                                │
 │  Linux: pthreads, futex, mmap, files, SDL3, PipeWire,        │
 │         dxvk-native → Vulkan (RADV)                          │
 └──────────────────────────────────────────────────────────────┘
```

## Repository layout (create as you go)

```
CLAUDE.md                agent rules (read first)
docs/PLAN.md             this file
docs/FINDINGS.md         RE facts
docs/LOG.md              session log
CMakeLists.txt
src/common/              winabi.h (WINAPI=ms_abi, Windows types), logging, handle table
src/loader/              pe_loader.cpp, teb.cpp, tls.cpp, seh/ (unwinder), main.cpp
src/hle/kernelx/         files, threads, sync, fibers, memory, time, iocp
src/hle/crt/             msvcr110 + msvcp110 (+ vccorlib110 in src/hle/vccorlib/)
src/hle/combase/         COM, HSTRING, activation factory registry
src/hle/winrt/           Windows.* and Windows.Xbox.* classes we implement
src/hle/d3d11x/          D3D11.X device/context on top of dxvk-native
src/hle/audio/           mmdevapi (WASAPI), acphal (XMA/SHAPE), xaudio2
src/hle/misc/            ws2_32, bcrypt, etwplus, mfplat, xg_x
third_party/             git submodules: dxvk (native), SDL3, (ffmpeg via system)
tools/                   scripts (analysis scripts read game_data, write to research/private)
tests/                   unit tests that don't need the game
game_data/               (gitignored) extracted dump
research/private/        (gitignored) anything derived from the game
```

## Decisions

- 2026-10-25: Azimuth is the reference for *architecture*, not just answers: compare its design (FINDINGS §6) before patching symptoms. Its zero-copy title memory is why it runs where our copy model keeps failing.

Add a line whenever a decision is made: `- YYYY-MM-DD: decision, reason`.

- 2026-10-23: Native ELF loader + HLE instead of Wine + shim DLLs. The user wants native Linux, and the API surface is small and known.
- 2026-10-23: Graphics via dxvk-native, not a custom Vulkan backend. The shaders are DXBC SM5, and DXVK already implements D3D11 → Vulkan.
- 2026-10-23: Build with clang + CMake + Ninja, C++20. Clang has solid `ms_abi` support.
- 2026-10-23: Windowing and input via SDL3. Audio output via SDL3 (PipeWire backend).
- 2026-10-24: DXVK pinned at v3.1.1, built by `tools/build_dxvk.sh` into `build-dxvk/install`. Only its libraries are linked into forza-linux; `src/hle/d3d11x` calls DXVK through raw vtables with the generated desktop slot enums (`tools/gen_d3d_slots.py`), because DXVK's Windows headers clash with `winabi.h`. DXVK headers are used only by `tests/dxvk_smoke.cpp`.
- 2026-10-24: Our immediate context copies the Xbox memory layout (embedded method table at +0x3D0, vptr pointing to it) and sets bit 1 of the byte at +0x390, so the game's inlined context methods call our table instead of poking state. Reason: the game inlines context methods (FINDINGS §3.1), and this is the switch the inlined code already checks.
- 2026-10-24: Placement (game-memory) resources are mirrored into DXVK resources with page write-tracking (mprotect + fault hook), uploading on bind. Reason: DXVK can't use foreign memory, and bind-time uploads with exact dirty pages are simple and correct for data the CPU writes.
- 2026-10-24: xg_x is reimplemented on AMD addrlib (vendored from Mesa, MIT) configured with the console's tile tables. Reason: assets are pre-tiled, so layouts must match the console's XG byte for byte; addrlib is what XG itself is built on.
- 2026-10-25: Azimuth (a proprietary Windows launcher that runs this game on D3D12) is studied as a behaviour reference under Rule 1, like the Microsoft DLLs: facts only, no code. Reason: the user asked for it, and it documents (via its strings and RTTI) exactly the subsystems we're blocked on.
- 2026-10-24: Package DLLs (Microsoft.Xbox.Services/GameChat/Input.Wheel from Mount/) are loaded and run, not reimplemented. Reason: they're the game's own dependencies and need only runtime imports we implement anyway; Xbox Live then runs offline.
- (open) Project license. LGPL-2.1+ makes it easy to borrow Wine code (unwinder, CRT bits). The repo has been public (github.com/MontyMcK/open-horizon-2, named forza-linux until 2026-10-27) since 2026-10-25 with no LICENSE file, i.e. all rights reserved by default: ask the user which one they want.

---

## Phase 0: setup and secrecy ✅

- [x] `git init`, `.gitignore` excludes `*.rar`, `game_data/`, `research/private/`, binaries, shaders, logs
- [x] `tools/check_no_game_data.sh` + pre-commit hook (`core.hooksPath=tools/git-hooks`)
- [x] `tools/extract_dump.sh`
- [x] Initial binary survey → docs/FINDINGS.md

## Phase 1: analysis tooling

Goal: be able to answer "what does the game call, from where, with which arguments" quickly.

- [x] `pip install --user pefile capstone`. `tools/pe_info.py` prints sections, imports, TLS, load config. Run Python with `-P`, not `-I`: `-I` hides the user site-packages.
- [x] `tools/rtti_dump.py`: walk MSVC x64 RTTI (CompleteObjectLocator → TypeDescriptor → ClassHierarchyDescriptor) and print `vtable RVA, class name, #methods` to `research/private/vtables.txt`. This is essential for naming D3D/WinRT interfaces later.
- [x] Ghidra 12.2 in `~/tools/ghidra_12.2_PUBLIC` (headless; `JAVA_HOME=~/.local/share/jdks/jdk-25.0.4.1+1`, `GHIDRA_HEADLESS_MAXMEM=24G` for the game exe). Projects: `research/private/game_ghidra` (fh2), `research/private/azimuth/ghidra`. `tools/ghidra/DecompileRefs.java` decompiles by `str:`/`fn:`/`addr:`/`callers:`/`vt:` query.
- [x] Full disassembly: `llvm-objdump -d --no-show-raw-insn --section=.text <exe> > research/private/text.asm` (~1 min). Grep it by address.
- [ ] Find the real start-up sequence: the entry point (CRT start) → `main`/`WinMain`-equivalent → `CoreApplication::Run(IFrameworkViewSource)`. Record the RVAs in FINDINGS.
- [ ] Find the file I/O path convention: which drive or prefix is used to open `media/...`. Search for the strings `media`, `G:\`, `T:\` and the CreateFileW call sites.
- [ ] Find **every** use of the three `d3d11_x` imports and record the argument structs (e.g. `D3D11X_CREATE_DEVICE_PARAMETERS`).

Done when: FINDINGS.md has the startup sequence, the path convention and the D3D11XCreateDeviceX parameter layout.

## Phase 2: loader MVP ("it executes the first instruction")

- [x] CMake skeleton, `forza-linux` executable, `src/common/winabi.h` (`#define WINAPI __attribute__((ms_abi))`, `HANDLE`, `DWORD`=uint32, `LONG`=int32, `WCHAR`=char16_t, `HRESULT`, `GUID`…), and a logging macro with per-module channels (`FL_LOG(kernelx, ...)`).
- [x] PE mapper: `mmap` SizeOfImage at `0x140000000` with `MAP_FIXED_NOREPLACE` (fall back to relocating), copy headers and sections, set page protections, apply `.reloc` if rebased.
- [x] Import resolver: an HLE registry `map<dll_lowercase, map<name|ordinal, void*>>`. For **unknown** imports, generate a per-import trap thunk (a tiny code stub that calls `unimplemented(dll, name)` and aborts with a clear log line). All 648 imports must resolve to *something*, so the first missing one is obvious at run time.
- [x] TEB: allocate a Windows-layout TEB (and a minimal PEB) per thread. Set GS with `arch_prctl(ARCH_SET_GS, teb)`, or `wrgsbase` if FSGSBASE is enabled; check `/proc/cpuinfo` and `AT_HWCAP2`. Fill Self (0x30), StackBase/StackLimit (0x08/0x10), FiberData (0x20), ThreadLocalStoragePointer (0x58), PEB pointer (0x60), and LastError (0x68).
  - glibc uses FS, not GS, so GS is free. **Our own C++ code must never touch GS.**
- [x] Static TLS: from the `.tls` directory, allocate the TLS block per thread, copy the template, set `_tls_index` and the slot array at `gs:0x58`, and run TLS callbacks (if any) on thread attach.
- [x] Jump to the entry point on a new thread with a big stack (e.g. 8 MB, 16-byte aligned plus Windows shadow-space rules) through an ms_abi trampoline.
- [x] `--game-dir <path>` argument. No hard-coded paths (Rule 0).

Also done: a crash handler (prints registers and game RVAs, and scans the stack for game return addresses), **data-import traps** (unimplemented data imports point at PROT_NONE pages, and touching one reports its name), and `FL_TRACE_IMPORTS=1|all`.

Done when: running it logs the first several hundred import calls (each import logs its name the first time it is hit), and it stops at a clear `UNIMPLEMENTED` instead of segfaulting.

## Phase 3: CRT + kernelx + exceptions ("reaches the game's main")

Implement imports in the order the game hits them. Rules of thumb:
- MSVCR110: map to libc, **but** convert types (32-bit `long`, UTF-16 `wchar_t` → write our own `wcs*` functions, `errno` per thread via the TEB, `FILE*` as our own struct, `_iob`). Heap: `malloc`/`free` directly, and `_msize` via `malloc_usable_size`.
- MSVCP110: `_Mtx_*`, `_Cnd_*`, `_Thrd_*` → pthreads. `std::_Lockit`, locale facets (`_Getctype`, `_Getcvt`...), `_Xlength_error` and similar throw helpers (these must *throw MSVC C++ exceptions*, see below). Wine's `msvcp90`/`msvcp110` code is a good reference.
- kernelx: a handle table (events, mutexes, semaphores, threads, files, IOCPs, fibers) with a single **futex-based wait implementation** that supports `WaitForMultipleObjects(Ex)` with wait-all/any, timeouts and alertable waits. Files map Windows paths to `--game-dir` (read-only media) and `~/.local/share/forza-linux/` (writable). Paths are case-insensitive, since the game's casing may not match the files on disk.
- Fibers: `CreateFiber`/`SwitchToFiber`/`ConvertThreadToFiber`. Use a hand-written asm context switch that saves the **Windows x64 callee-saved registers, including xmm6–xmm15**, and updates TEB StackBase/StackLimit/FiberData. Don't use `ucontext`.
- XMem* / VirtualAlloc: honour the Xbox-specific allocation attributes as plain `mmap`. Log any unknown flag bits (Xbox has MEM_GRAPHICS / MEM_TITLE variants). Keep an allocation map, because D3D will later need to know which ranges are "GPU memory".
- **Exceptions**: the C++/CX code throws (`Platform::Exception`), and MSVC C++ EH uses `RaiseException` → `RtlUnwindEx` → `__CxxFrameHandler3` (exported by MSVCR110, which we provide).
  - [x] Implemented in `src/hle/seh/`: the virtual unwinder (prolog/epilog/chained), the two-phase dispatcher, `__CxxFrameHandler3`, `_CxxThrowException` with rethrow, `__C_specific_handler`, and exception_ptr. Catch funclets run "deep", with an active-catch stack that lets the unwinder skip dead frames (see the comments in seh.h and cxx_eh.cpp).
  - [x] The unwinder is validated on real game code: crash backtraces walk cleanly to the thread entry.
  - [ ] **Not yet exercised by a real game throw.** Watch the first one closely (`FL_LOG=seh`). Also still missing: `__abi_WinRTraise*Exception` (vccorlib), which needs a Platform::Exception object + ThrowInfo built in our memory.
  - [ ] Map SIGSEGV/SIGILL/SIGFPE inside game code to SEH dispatch (later, only if the game relies on `__try`).
- [x] `QueryPerformanceCounter` → `CLOCK_MONOTONIC` (pick a 10 MHz frequency).
- [ ] `SetThreadAffinityMask` / ideal processor: accept and log. The Xbox has 6–7 game cores, so don't pin.

Status (2026-10-23): kernelx is implemented 112/112 (files+IOCP, threads+suspend, fibers with an asm context switch, waits, critical sections, SRW locks, VirtualAlloc, XMem hooks). Still a trap: `TitleMemoryStatus` (struct layout unknown). msvcr110 has startup, strings, math, stdio with an MSVC printf/scanf engine, std::exception/type_info/RTTI, and ConcRT locks/events/scheduler/concurrent_queue. **Not done:** C++ EH/SEH (`_CxxThrowException`, `__CxxFrameHandler3`, `__C_specific_handler`, exception_ptr), PPL task collections and cancellation tokens, most of msvcp110 (iostreams/locale).

Done when: execution reaches the game's own entry (the C++/CX `main` that creates the `Forza.App` view source) with no crashes.

## Phase 4: COM/WinRT shell ("the game loop is running")

Status (2026-10-23): **done enough to reach the game loop.** CoreApplication::Run drives IFrameworkView (Initialize/SetWindow/Load/Activated/Run). CoreWindow and CoreDispatcher are headless; the Xbox ICoreWindow layout differs from desktop (FINDINGS §4b). Use `src/hle/winrt/object.h` for new classes: a `Vtable` whose unknown slots trap with "Iface slot N". **Rule: only fill slots verified at a call site.** A wrong desktop guess (get_Bounds written into a get_Dispatcher pointer) silently corrupted the stack once. Also stubbed: PackageTransferManager, WASAPI (null backend), the Xbox HW channel-count interface, offline ws2_32, and C++/CX events, weak refs and delegates.
Still open: input (`Windows.Xbox.Input.*`), User/Live/Storage classes (the game hasn't asked yet), CoreWindow backed by a real SDL3 window, firing VisibilityChanged/key events.

- [ ] combase: HSTRING (refcounted UTF-16), `CoInitializeEx`, `CoCreateInstance` (registry of CLSIDs → our factories), `CoCreateFreeThreadedMarshaler` (trivial IMarshal), and apartment/context functions returning sensible values.
- [ ] vccorlib110: Platform::Object/String/Exception/Delegate/event source helpers, and `GetActivationFactoryByPCWSTR` → our activation registry. Check Wine's `vccorlib` (in `dlls/vccorlib140`) for reference.
- [ ] A WinRT authoring helper: a C++ template that builds ms_abi vtables for `IInspectable`-derived interfaces, so each new class is quick to add.
- [ ] `CoreApplication::Run(IFrameworkViewSource)`: call `CreateView` → `Initialize(CoreApplicationView)` → `SetWindow(CoreWindow)` → `Load` → `Run`. CoreWindow is backed by an **SDL3 window**. `CoreDispatcher::ProcessEvents` pumps SDL events.
- [x] `Windows.Xbox.System.User`: a single fake signed-in local user ("Player").
- [x] `Windows.Xbox.Input.Controller` / `IGamepad` / `IGamepadReading`: SDL3 gamepad (DualSense verified), with keyboard fallback. `FL_AUTOPRESS="30:A,33:Menu"` scripts presses for unattended tests. Wheel support comes later.
- [x] `ConnectedStorageSpace` → `~/.local/share/forza-linux/saves/<xuid>/<container>/<blob>` with atomic container commits (connected_storage.cpp, FINDINGS §4f). ReadAsync / property sets / blob queries still trap (unused so far).
- [ ] Xbox Live, Party, Multiplayer, Achievements, Store, Kinect, Speech, GameChat, ClipCapture: **offline stubs** that report "not signed in / not available" through the documented failure paths (`E_FAIL`/async errors), not crashes.
- [x] Async operations (`IAsyncOperation<T>`, `Completed` handlers): async.cpp, completed before they are returned.
- [x] **Offline Xbox Live:** token requests fail with 0x800704CF like Azimuth; the session job's task-based continuation catches it once C++/CX exceptions keep their dynamic type (FINDINGS §4g, §5a). Still open: SecureDeviceAssociationTemplate (offline template from the manifest, as Azimuth) and AchievementNotifier.

Done when: the game's main/render threads start, it loads data from `media/`, and it reaches its first `D3D11XCreateDeviceX` call.

## Phase 5: graphics (D3D11.X → dxvk-native → Vulkan)

The hardest phase. The order matters.

- [x] Add DXVK as a submodule and build it **native** (v3.1.1, SDL3 WSI, D3D11 + DXGI only): `tools/build_dxvk.sh`. `./build/dxvk_smoke` checks it works on this machine without the game. DXVK needs `DXVK_WSI_DRIVER=SDL3` (main.cpp sets it).
- [x] Find the real D3D runtime for RE: it's `umd.dll` (FINDINGS §3). `tools/pe_re.py` finds and prints its vtables.
- [x] Device vtable (84 slots) and the "extra base slot" rule: FINDINGS §3.1.
- [x] Scaffolding in `src/hle/d3d11x/`: `D3D11XCreateDeviceX` creates a DXVK device; DeviceX / DXGI device / adapter / output wrappers; ContextX with the Xbox block layout. Every slot we haven't verified traps with `UNIMPLEMENTED ID3D11DeviceX!slot N` or `ID3D11DeviceContextX!slot N (ctx+0xOFF)`.
- [x] Device slot 59 (DMA/copy-engine context): created; its own methods still trap until used.
- [x] Context slot map for all 261 slots (FINDINGS §3.1) + all desktop-equivalent context methods (context.cpp), inline topology, compact commands (slot 209), deferred contexts + command lists.
- [x] Resources, views, shaders, states, queries (resources.cpp, pipeline.cpp), placement buffers/textures + write-tracked uploads (placement.cpp, gpumem.cpp), D3DMapEsramMemory.
- [x] xg_x: tile modes, layouts, texture computers (src/hle/xg), GCN untiler via addrlib, verified against real assets.
- [x] Swap chain on an SDL3 window (dxgi.cpp, platform/window.cpp), DXGIXPresentArray, screenshots.
- [ ] Context Xbox-only slots 144–260 as the game hits them. Names for all of them: FINDINGS §3.1 (from Azimuth). Done: 151–155, 160–177, 196, 197, 209, 214; next likely 178–193 (placement binds by CPU pointer).
- [x] **Xbox draw ABI**: DrawIndexed/DrawIndexedInstanced/DrawInstanced pack argument pairs into 64-bit registers (FINDINGS §3.1); fixing it made the title screen's video and UI appear.
- [x] DXVK/RADV crashes around 31 s: the game overran a 3D texture because our XG reported a whole-level `Slice2DSizeBytes` (fixed), corrupting the C heap that XMem blocks shared with DXVK (XMem now gets its own mappings); plus immediate-context use from two threads (DXVK lock on).
- [x] Frame trace: `FL_TRACE_FRAME=N[,M]` logs every draw/dispatch/copy/clear/map/upload with bound shaders, textures and targets; `FL_TRACE_DUMP=1` also saves inputs and targets as PNGs in `screenshots/trace/`.
- [ ] The R8_UINT SRV on an R11G11B10_FLOAT texture that DXVK rejects (game keeps going; find the call site and what it's for).
- [x] Depth+stencil XG layouts: [HTILE] DEPTH [STENCIL] with xg_x's usages and HTILE size (FINDINGS §3.2).
- [ ] XG colour render targets: CMASK (+FMASK) planes after the colour plane (xg_x rva 0x1770, FUN_180005220); STUB today.
- [x] **Draw bundles:** deferred contexts created with flag bit 17 inherit the executing context's state: DXVK patch tools/dxvk-patches/0001-xbox-draw-bundles.patch (FINDINGS §5b). The 3D scene now renders.
- [x] **Tonemap writes 0:** not a rendering bug: the camera fade stays black because the intro waits for FMOD streams (FINDINGS §4). With the wait forced (debug) the game fades in and reaches the world.
- [ ] Deferred/G-buffer geometry: lit objects are black silhouettes and the main depth reads 1.0 at post-processing.
  - [x] Shadow maps were never cleared (DMA fills): fixed, foliage is lit now.
  - [x] Write tracking lost writes to small buffers sharing a page (page → resources index, `force_dirty`).
  - [x] Bundles recorded stale placement uploads (re-sync at ExecuteCommandList).
  - [x] SetShaderResourceViewMinLOD (slot 223) emulated with a clamped view.
  - [x] **Near terrain / car / roads not drawn**: bundles bound 1x1 dummy render targets (bundle OMSetRenderTargets must be shadow-only).
  - [x] **Scrambled materials**: bundle inheritance remaps (slots 156-159, DXVK patch 0002).
  - [x] Black shadows / black building: DXVK SRV hazard unbinding broke the game's probe mip chain (patch 0003).
  - [ ] Sky shows coarse banding (top-left of the gameplay frame).
- [ ] ESRAM aliasing: resources share ESRAM offsets (FINDINGS §5b); find out whether any pass reads through an alias what another wrote.
- [x] Untiler speed: per-layout cached plans (tile bases + in-tile pattern, checked against addrlib) and row bands on a worker pool, 10-50x faster (2026-10-27).
- [ ] GPU→CPU: placement memory the GPU writes (render targets the CPU reads back) is not synced back.
- [ ] Unified memory: on Xbox the CPU and GPU share RAM, and the game may write GPU data directly (placement resources, `D3DMapEsramMemory`, "fast semantics" with no Map/Unmap).
  - Plan A: back game "graphics" allocations with Vulkan host-visible memory, or import them with **`VK_EXT_external_memory_host`** (RADV supports it), so GPU buffers alias the game's pointers.
  - Plan B: shadow copies plus dirty tracking (write-protect pages, then sync before each draw). Slower but simpler.
- [ ] Textures: if the game uses `xg_x` layouts / GCN tile modes, write an **untiler** (CPU first, compute shader later). Check what tile mode the game requests (Phase 1).
- [x] Present: `DXGIXPresentArray` → DXVK swapchain on the SDL window. (1080p/900p scaling still to check.)
- [x] **Screenshot automation** (needed so agents can verify progress): env `FL_SCREENSHOT_AT_FRAME=N` dumps the swapchain to `screenshots/frame_N.png` (gitignored), and `FL_EXIT_AFTER_FRAME=M` quits. Agents then read the PNG to check progress.
- [ ] RenderDoc capture support: works with DXVK, so document how to use it in LOG.

Milestones: (a) clear-colour frame, (b) the 2D boot logos/menus, (c) the 3D garage/car, (d) driving in the open world.

## Phase 5b: memory model like Azimuth (FINDINGS §6) — the current direction

Azimuth runs the game because the GPU reads the game's memory directly. Our copy-and-track model is what keeps breaking. In order:
- [x] **Change tracking without mprotect.** gpumem now uses userfaultfd async write-protect + `PAGEMAP_SCAN` (the Linux GetWriteWatch). No fault hook, kernel writes are recorded. `FL_CHECK_TRACKING=1` hashes every skipped upload to catch misses.
- [x] DMA engine context: every slot the game uses (9, 10, 16–19, 21, 22, 24; FINDINGS §3.1) runs on the CPU or on the immediate DXVK context (dma.cpp).
- [x] DMA fills into GPU-owned placement depth/stencil planes become ClearDepthStencilView (dma.cpp `fill_gpu_owned`; the per-frame shadow-map clears). HTILE fills are dropped.
  - [ ] Fills of GPU-owned colour planes, partial plane fills, and CopyMemoryToMemory over GPU-owned memory: logged UNIMPLEMENTED, not seen yet.
- [ ] **Zero-copy placement buffers.** Import the game's memory into Vulkan with VK_EXT_external_memory_host (RADV supports it) and create the DXVK buffer over it: needs a small interop entry point in our DXVK build (third_party/dxvk), page-aligned import ranges (title memory reserved in 64 KB-aligned chunks), and GPU→CPU visibility for free. Decide with the user: patch DXVK, or move to D3D12 on vkd3d-proton like Azimuth (`OpenExistingHeapFromAddress` exists there).
- [ ] ESRAM with a real shared backing (memfd + MAP_SHARED views) so aliased mappings agree.
- [ ] Title memory bookkeeping: one reservation for graphics memory; releases withdraw GPU heaps over them (Azimuth "withdraw the heaps over title memory").

## Phase 6: audio and video

- [x] `ActivateAudioInterfaceAsync` → our `IAudioClient2`/`IAudioRenderClient`/`IAudioClock`, backed by an SDL3 audio stream (48 kHz float stereo, PipeWire). FMOD's output path (FINDINGS §4). `FL_AUDIO=null` drops audio.
- [x] AcpHal heaps, SHAPE context arrays, IAcpHal command/message queues with a 128-sample ACP clock (FINDINGS §4).
- [x] **Blocks the intro:** FMOD XMA streams must load (SHAPE emulation, shape.cpp; consumed as silence without FFmpeg)
- [ ] Build FFmpeg XMA (`tools/build_ffmpeg_xma.sh`, needs the user: the agent may not compile downloaded code), wire FL_HAVE_FFMPEG_XMA into CMake, test real XMA audio.
- [ ] (old) Blocks the intro: FMOD XMA streams must load (EventInstance::getStreamLoadingState == LOADED) or the IE cutscene never becomes ready (FINDINGS §4). Needs the SHAPE/XMA emulation below.
- [ ] SHAPE flowgraph execution: decode the SHAPE commands FMOD puts in its 0x3000-byte flowgraph buffer and the 64-byte XMA contexts, then decode **XMA2** with FFmpeg's `xma2` decoder (Azimuth uses a modified FFmpeg: one stream per decoder, every frame output immediately; Fedora's ffmpeg-free has no xma1/xma2/wmapro decoders (checked 2026-10-26 with avcodec_find_decoder_by_name): build a minimal FFmpeg with only those (LGPL, like dxvk in third_party)).
- [ ] `xaudio2_9.dll` (dynamically loaded): provide it from the HLE registry via **FAudio**, which implements the XAudio2 API, if the game actually loads it.
- [ ] Media Foundation `MFCreateSourceReaderFromURL`: find out what it plays. Stub or implement with FFmpeg.
- [ ] Bink runs as game code, so it should just work once audio and textures work.

## Phase 7: playable and pleasant

- [ ] Racing wheel (`Microsoft.Xbox.Input.Wheel`) → SDL joystick/haptics
- [x] Frame pacing: 30 fps on a virtual 59.94 Hz vblank on any monitor (the update steps a fixed 1/29.97 s per frame); display modes (FL_DISPLAY_MODE=windowed|borderless|fullscreen, F11/Alt+Enter); launcher settings (2026-10-27).
- [x] Dynamic frame rate at real-time game speed (FL_FRAME_PACING=dynamic, FL_FPS_CAP; 2026-10-27).
- [x] Reach 120 fps in the open world: ~175 fps (DXVK patches 0007/0008, DMA fill lookup; 2026-10-27).
- [ ] Render thread CPU (the limit at ~5 ms/frame): ~10k CS chunks per frame from bundle remaps; page-tracking ioctls; uneven frame times (p10 3.8 / p90 ~9 ms).
- [ ] Dynamic / uncapped frame rate at real speed: the update step is rva 0xf58770, the engine options are `synchroniseUpdateToRenderThread` / `unlockframerate` (FINDINGS §6c). Find how options reach the parser (rva 0x299950), or patch the `je` at rva 0xf587dd. The UI clock also steps per frame (Azimuth's "UI step"/"UI clock tick").
- [ ] Settings: resolution, FOV
- [x] Write tracking per frame instead of per bind (placement "frame scan", hot/cold resources), ~4.7 → ~1 ms per frame; yield backoff for the game's spin semaphores (~2 cores less at 30 fps) (2026-10-27).
- [ ] Streaming hitches left: rare 40-70 ms frames while big textures arrive, and the world load (~80-190 ms frames). Next: upload big textures from a worker (untile + UpdateSubresource off the render thread).
- [ ] Performance: profile with `perf` (the game code shows up as anonymous memory, so add a perf map file `/tmp/perf-<pid>.map` with RTTI names)
- [ ] Packaging: `forza-linux --game-dir /path/to/Mount`, plus `tools/extract_dump.sh`. A user supplies their own dump, and nothing from the game ships.
- [ ] Optional: online and Live features stay stubbed

---

## Debugging tips (append as you learn)

- Run under `gdb`. The game code has no symbols, so use `info symbol` on our side and RVAs (`addr - 0x140000000`) on the game side, then look them up in Ghidra.
- Tell gdb not to stop on signals we use internally: `handle SIGSEGV nostop noprint` only once SEH mapping exists.
- `FL_LOG=all` / `FL_LOG=kernelx,d3d` to choose log channels. Logs go to `runtime/` (gitignored).
- When the game crashes in its own code, the cause is almost always a wrong return value or struct layout from our HLE. Check the last ~20 import calls in the log first.
