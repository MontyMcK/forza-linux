# Session log

Append-only. Newest entry at the bottom. Format:

```
## YYYY-MM-DD: <agent/model>: <short title>
Did: ...
Works: ...
Broken / open questions: ...
Next step: <one concrete action>
```

---

## 2026-10-23: Claude Opus 5.5: project bootstrap

Did:
- Inspected the dump (`forza 2.rar`, RAR5, 39 GiB). It is a decrypted Xbox One package of FH2 (codename "Anthem"): `Mount/` (game) + `EmbeddedXvd/` (ERA title OS).
- Extracted only the exe, the manifest and a few sample files into `game_data/` (gitignored). The full extract is ~40 GB; do it when needed with `tools/extract_dump.sh`.
- Surveyed the exe: imports, sections, AVX use, GS/TEB usage, RTTI, shader format (DXBC SM5), audio (FMOD Studio + XMA via AcpHal). Everything is in docs/FINDINGS.md.
- Set up git with a strict `.gitignore`, the `tools/check_no_game_data.sh` pre-commit hook, CLAUDE.md (rules) and docs/PLAN.md (roadmap).

Works: secrecy guard (`git check-ignore` confirms the rar and game_data are ignored).

Broken / open questions:
- How the game opens `media/` files (drive letter / relative path)?
- Does it upload pre-tiled textures (xg_x)? This decides how much untiling work Phase 5 needs.
- The project license hasn't been chosen yet (matters if Wine code is borrowed).

Next step: Phase 1, first task: install `pefile`, write `tools/pe_info.py`, and dump the TLS directory and the load config.

## 2026-10-23: Claude Opus 5.5: loader + kernelx + CRT; the game reaches main

Did:
- Phase 1 tools: `tools/pe_info.py`, `tools/rtti_dump.py` (31,797 vtables in <1 s). Full disassembly goes to `research/private/text.asm`.
- Phase 2: loader (`src/loader/`). It maps at 0x140000000, applies relocs, binds imports to the HLE registry or to named traps, sets up the TEB via `arch_prctl(ARCH_SET_GS)` and static TLS, and has the crash handler and data-import traps.
- Phase 3: all 112 kernelx imports (`src/hle/kernelx/`); most of msvcr110 (`src/hle/crt/`), including an MSVC-compatible printf/scanf over `__builtin_ms_va_list`; ConcRT basics; vccorlib basics; combase.
- Run: `FL_LOG=loader,thread,kernelx,crt,winrt,concrt,file ./build/forza-linux --game-dir game_data/Anthem_1.0.0.64_x64__8wekyb3d8bbwe/Mount`

Works: CRT startup, static initializers, heap setup, the first game thread, and main up to CoreApplication activation.

Broken / open questions:
- **C++ exception handling does not exist yet.** The first `throw` dies with `UNIMPLEMENTED ...WinRTraise...`, `_CxxThrowException` or `RaiseException`. This is the next big piece of infrastructure (see the PLAN Phase 3 bullet "Exceptions").
- Gotcha: `grep -v section` hides FATAL lines that contain `critical_section`. Grep for `FATAL` explicitly.
- Our sigaltstack is only installed on the process main thread, so a stack overflow on game threads won't get a nice report.

Next step: implement CoreApplication / CoreApplicationView / CoreWindow (Phase 4) using a dynamic vtable builder whose unimplemented slots trap with "Interface slot N".

## 2026-10-23 (later): Claude Opus 5.5: SEH/C++ EH, WinRT shell; the game loop runs

Did:
- Extracted the full dump (42 GB) into `game_data/`.
- Fixed a hang: the game treats `CRITICAL_SECTION.DebugInfo != 0` as "initialized".
- Exception handling (`src/hle/seh/`): the unwinder, dispatcher, `__CxxFrameHandler3`, `__C_specific_handler`, `_CxxThrowException`, exception_ptr. Crash reports now print a real `.pdata` backtrace.
- WinRT: a dynamic-vtable object framework (`src/hle/winrt/object.*`), CoreApplication/View/Window/Dispatcher, C++/CX event sources, weak refs, delegates. Plus PackageTransferManager, the Xbox resource-availability statics, WASAPI with a null audio backend, the Xbox HW channel-count interface, and offline Winsock.
- msvcp110: container bases, `_Lockit`, `_X*` helpers. ConcRT `concurrent_queue`.

Works: the game runs Initialize → SetWindow → Load → Run. The main loop pumps `ProcessEvents(3)` while ~9 game threads run (job workers, audio, streaming), and the render thread calls `D3D11XCreateDeviceX`.

Broken / open questions:
- No real C++ throw has happened yet, so the EH code is untested on a live exception.
- `WinRTraise*` (C++/CX exceptions from vccorlib) is not implemented.
- Graphics needs dev packages that aren't installed yet: SDL3-devel, vulkan-headers, vulkan-loader-devel, meson, glslang, ffmpeg-free-devel (or ffmpeg-devel).

Next step: Phase 5. (1) Get the packages installed; (2) add DXVK as a submodule and build it native; (3) RE `D3D11X_CREATE_DEVICE_PARAMETERS` and the first ID3D11DeviceX/ContextX slots the game calls, starting with a device whose slots all trap.

## 2026-10-24: Claude Opus 5.5: DXVK built; D3D11.X device on DXVK; Xbox D3D layouts mapped

Did:
- The user installed the dev packages (SDL3-devel, vulkan-headers, vulkan-loader-devel, meson, glslang, ffmpeg-free-devel).
- DXVK v3.1.1 added as submodule `third_party/dxvk`, built native (SDL3 WSI, D3D11 + DXGI) with `tools/build_dxvk.sh` into `build-dxvk/install`. `tests/dxvk_smoke.cpp` (`./build/dxvk_smoke`) creates a device on the RX 9070 XT (RADV GFX1201), clears a texture and reads it back: OK.
- Found the real D3D runtime: `EmbeddedXvd/.../umd.dll` (d3d11_x.dll only loads it). Mapped the device (84 slots), DXGI device (18), the context's embedded method table and the inlined-context mechanism. Everything is in FINDINGS §3.1. Wrote `tools/pe_re.py` for vtable work on DLLs without RTTI.
- `src/hle/d3d11x/`: `D3D11XCreateDeviceX` creates a DXVK device. DeviceX (with ID3D11DeviceX/IDXGIDevice ifaces), adapter and output wrappers, and ContextX (Xbox block layout, table at +0x3D0, mode bit set). `desktop_slots.h` is generated by `tools/gen_d3d_slots.py`.
- forza-linux now links libdxvk_d3d11/libdxvk_dxgi (CMake fails with a hint if `tools/build_dxvk.sh` hasn't been run). main.cpp sets `DXVK_WSI_DRIVER=SDL3`.
- Staged all new files (no commits exist yet; the user hasn't asked for one).

Works: the game's device setup runs on DXVK: CreateDevice, the QIs (including three Xbox-only IIDs), GetAdapter, EnumOutputs.

Broken / open questions:
- Stops at `UNIMPLEMENTED ID3D11DeviceX!slot 59` (game rva 0x10d8b01): creating what is probably a DMA/copy-engine context (FINDINGS §3.1).
- Context slot signatures can differ from desktop (slot 20). Verify each at a game call site.
- Unguarded inline writes to ctx+0x170/+0x168 exist; not handled yet.
- DXVK prints "External memory features not supported" (that's D3D11 shared resources; VK_EXT_external_memory_host for Plan A is a separate question, not checked yet).
- GetCreationFlags returns the D3D11XCreateDeviceX flags (STUB, unverified).

Next step: implement device slot 59. Run `python3 -P tools/pe_re.py slots <umd.dll> 0x16f580 0-29 30` to study the 30-slot object, build a wrapper whose unknown slots trap (like `output_vt()` in device.cpp), return it from slot 59, and rerun:
`FL_LOG=d3d ./build/forza-linux --game-dir game_data/Anthem_1.0.0.64_x64__8wekyb3d8bbwe/Mount`

## 2026-10-24 (later): Claude Opus 5.5: graphics on DXVK, first frame, xg/untiler, user, package DLLs

Did:
- Mapped the whole Xbox context table (FINDINGS §3.1) and implemented every desktop-equivalent context method, the inlined-topology readback, slot 209 compact commands, deferred contexts/command lists; resources, views, shaders, states, queries; DXGI adapter/output/factory/swap chain; DXGIXPresentArray; D3DMapEsramMemory.
- Placement resources (device slots 62–65) mirrored into DXVK with page write-tracking (gpumem).
- xg_x on addrlib (third_party/addrlib from Mesa, MIT): tile mode heuristics, layouts, texture computers; untiler verified on a real texture (tests/gcn_layout_test, tools/swatch_view).
- SDL3 window created on the UI thread (platform/window.cpp; Vulkan loaded before the render thread starts DXVK).
- Windows.Xbox.System.User (one signed-in "Player") using interface data decoded from gameosproxy.dll's MIDL proxies (tools/midl_proxies.py, FINDINGS §4c).
- PE DLL loading for package DLLs (loader/modules.*): image registry used by SEH/crash reports, LoadLibrary/GetProcAddress, manifest-based activation via DllGetActivationFactory.

Works: device + swap chain creation, ~30 placement resources, XG layouts, **first DXGIXPresentArray / Present**, user sign-in, Microsoft.Xbox.Services.dll maps and starts DllMain.

Broken / open questions:
- Microsoft.Xbox.Services.dll needs 83 more imports (list: research/private/pkg_imports.txt vs HLE exports): C++/CX runtime (Platform::Exception family, ToString, casts, Platform::Details::GetActivationFactory), PPL (_TaskCollection, _CancellationTokenState), msvcp110 ctype<wchar_t>/locale, a few CRT/kernelx/combase functions. First one hit: `_malloc_crt`.
- The frame-1 screenshot hasn't been written yet (the process aborts on another thread first).
- See PLAN Phase 5 open items.

Next step: implement the simple missing imports first (CRT/kernelx/combase), then Platform::Details::GetActivationFactory (RE how the DLL's DllGetActivationFactory calls it), then C++/CX exceptions and PPL.

## 2026-10-25: Claude Opus 5.5: iostreams, audio stack, title screen, input, saves, public repo, Azimuth reference

Did:
- msvcp110 iostreams/locale (src/hle/crt/msvcp_iostream.cpp, msvcp_locale.cpp, msvcp_ios.h; FINDINGS §4e).
- Audio: AcpHal heaps/SHAPE contexts/IAcpHal queues (src/hle/acphal), WASAPI via ActivateAudioInterfaceAsync + IAudioClient2 on SDL3 (src/hle/audio/wasapi.cpp), MediaDevice (src/hle/winrt/media_devices.cpp). FMOD runs; XMA voices are silent (flowgraphs acknowledged, not executed).
- D3D: Xbox-only context slots 151–155/160–177/197/214; **the Xbox draw ABI fix** (packed 64-bit arguments, FINDINGS §3.1): the title screen now renders correctly (Bink video, logo, A glyph). Copy sources sync placement memory like binds. Frame tracer + texture dumps (src/hle/d3d11x/trace.*).
- vccorlib: Object::Equals, ToString family, Type, GetObjectContext, TerminateModule, NullReferenceException, Exception::Message, FailFast. PPL tasks: cancellation tokens and task collections on the ConcRT pool (concrt.cpp, FINDINGS §5a); native catch-all boundary `seh::guarded_call` for chores.
- Input confirmed with the user's DualSense; `FL_AUTOPRESS` for unattended tests; `FL_LOG=input` logs button changes.
- ConnectedStorage on local files with atomic commits + Windows.Storage.Streams.Buffer (src/hle/winrt/connected_storage.cpp, FINDINGS §4f). The game creates its profile on first A press.
- User::GetTokenAndSignatureAsync answers "offline" (0x800704CF), like Azimuth.
- Crash reports: game backtraces for unhandled exceptions; dladdr names for native crashes. `FL_LOG=missing` lists every unimplemented import (30 left, list in research/private/missing_all.txt).
- Public repo https://github.com/MontyMcK/forza-linux (one commit; repo-local no-reply identity; no LICENSE yet: ask the user).
- Azimuth (proprietary launcher, user-supplied) is studied as a reference (CLAUDE.md Rule 1): Ghidra 12.2 headless, projects and `tools/ghidra/DecompileRefs.java`. Useful already: the D3D11.X slot-name table, ContainerInfo layout, GetAsync error code, the offline token answer, NetworkInformation answer.

Works: boot → intros → title screen with video and UI; DualSense; saves; audio output path.

Broken / open:
- **Blocker:** after A, the game's job at rva 0x1839500 waits on a sessiondirectory.xboxlive.com query with an unguarded `task.get()`; offline the query fails and the rethrow is unhandled (FINDINGS §4g). The job has an offline check: if `0x327270()->vfn 0x48 → vfn 0xb0 → vfn 0x128` is null it skips the query (sets 0x4cd).
- An intermittent native SIGSEGV around 31 s (once; next report will name the library).
- The game's Ghidra project was re-run with a 24 GB heap (the 2 GB default ran out of memory); check `research/private/game_ghidra_import.log` for "REPORT: Save succeeded".

Next step: with the game's Ghidra project, run `DecompileRefs.java` on `addr:0x1839500`, `addr:0x327270`, `callers:0x183bed0` (see the header of tools/ghidra/DecompileRefs.java for the command; the program name is `forza_durango_release_final.exe`, project `research/private/game_ghidra/fh2`). Identify the object behind vfn 0x48/0xb0/0x128 (probably the game's Xbox Live/online-session wrapper) and what makes it null on an offline console; then make our runtime produce that state (likely a WinRT answer we get wrong). Check Azimuth's answers for the same calls if stuck.

## 2026-10-25 (later): Claude: past the A press into world loading; why Azimuth runs and we don't

Did:
- Hang after A was the session-directory "pending" experiment. Real cause of the original crash: our C++/CX throws used the static type, so the game's `catch (COMException^)` in a task-based continuation never matched (FINDINGS §4g, §5a). Fixed in vccorlib (WINRTEXCEPTIONINFO before the object) and `_CxxThrowException` (TI_IsWinRT). Experiment removed; offline token requests all fail like Azimuth.
- Hang dump: `FL_DUMP_THREADS_AT=<s>[,<s>]` (every thread's native symbol + game frames), `FL_CRASH_DUMP_THREADS=1`, `FL_CRASH_PEEK=[@]<rva>:<off>` (game globals in crash reports), crash reports now name the fault address's mapping and why the gpumem hook declined.
- DXVK/RADV crash at ~31.7 s: heap corruption. The game overran its 3D colour LUT because XG reported a whole-level slice size (gcn.cpp fixed); the overrun hit the C heap shared with DXVK because XMemAllocDefault used posix_memalign (now dedicated mmaps). Also the immediate context is used from two threads (DXVK ID3D10Multithread lock on).
- gpumem: kernel reads into tracked memory go through a bounce buffer (EFAULT race), protection changes are serialized with a spinlock, kernelx commit/decommit/release keep the tracker in sync, released memory is forgotten and re-tracked on reuse.
- Audio: `SetWasapiThreadAffinityMask` returned 0, so FMOD's init failed and the game fell back to its silent engine, which crashes the world load. Now returns the previous mask (FINDINGS §5b); FMOD starts its WASAPI output. IPackageTransferManager S+0, context slots 156–159 added.
- Skill `.claude/skills/decomp/SKILL.md` (when stuck: decompile the game, Azimuth, the reference DLLs), `tools/eh_info.py`.
- Azimuth architecture study (FINDINGS §6): it maps the game's memory straight into the GPU (D3D12 heaps over title memory, vkd3d-proton beneath), detects changes with OS write-watch / content comparison, backs ESRAM for real and decodes XMA2. That's the difference.

Works: boot → title → A → profile → past the loading screen into world setup; a 120 s run with FL_TOKEN_FAIL_ALL behaviour and real FMOD audio did not crash.
Broken / open: GPU writes to placement memory never reach the CPU; mprotect tracking remains fragile; XMA silent; ESRAM aliasing ignored. An occasional decline in the gpumem hook (a `vmovntdq` copy at rva 0x1081ebd faulting twice) is not understood yet.
Next step: decide the memory-model direction with the user (PLAN Phase 5b): patch DXVK for host-memory import vs. D3D12 over vkd3d-proton. Meanwhile, replace the mprotect fault hook with content comparison (cheapest Azimuth-like step).

Correction: the "120 s without a crash" runs above were the game stuck on a black screen after the intro (see below), not world loading.

## 2026-10-25 (evening): Claude: write-watch tracking; title screen with real FMOD; past A into DMA

Did:
- gpumem now uses userfaultfd write-protect (async) + `PAGEMAP_SCAN`, the Linux equivalent of Windows' GetWriteWatch that Azimuth uses. No SIGSEGV hook, kernel writes work, no races (FINDINGS §6). Removed the bounce buffer, protection_changed/forget, decline diagnostics.
- With FMOD actually running, the screen stayed black after the intro. Traced with new tools to: the title waits for the Splash's "finished" flag, set when the `BASE_PressStartMusic` mixer snapshot starts, which needs MasterBank.bank, which the game's bank loader refuses because its installed-chunk set is empty for the audio chunks. We now read the package's chunks from `Mount/hash.manifest` and report them all through the whole-package PackageTransferWatcher's ChunkCompleted event (FINDINGS §5b). MasterBank loads, the title shows (video, logo, "press A"), A starts the profile/Live flow.
- Context slot 154 InsertFence; DMA engine context slots 17/18/21/22 (copies/fills on the CPU; GPU-only uncommitted destinations skipped).
- New diagnostics (crash.cpp): `FL_BREAK=<rva>,…` one-shot breakpoints; `FL_BREAK_REPEAT=<n>` (re-armed via single-step, logs rcx/rdx/rax), `FL_BREAK_STR` (rdx as std::string), `FL_BREAK_CSTR`/`FL_BREAK_RCXSTR=<off>`/`FL_BREAK_RBXSTR=<off>` (C strings), `FL_BREAK_DEREF=<off>` (object at [rcx+off] and its vtable); `FL_BREAK_ALL=<seconds>` differential coverage (one-shot breakpoint on every primary .pdata function, logs first hits after that time: chained entries can start on jump tables, so they are skipped); `FL_WATCH=<fn rva>:<off>` write watchpoint on this+off; `FL_CRASH_PEEK`, `FL_DUMP_THREADS_AT` peeks; `FL_NO_FMOD=1` forces the silent audio engine (A/B comparisons). The differential-coverage + watchpoint pair found the title gate in a few runs.

Works: boot → intro → title with FMOD audio → A → profile/Live → loading (until the next missing piece).
Next step: rerun with the DMA changes (the run after the DMA fill fix was not done when the shell went unavailable) and continue: whatever the game needs next while loading the world.

## 2026-10-26: Claude: DMA engine, XG metadata planes, streamed textures; 3D frame renders black (bundles)

Did:
- DMA context slots 9 CopyResource, 10 CopySubresourceRegion (recorded on the immediate DXVK context), 16 InsertFence, 19 WriteStatus, 24 LZDecompressMemory (zlib via system zlib; Azimuth evidence in FINDINGS §3.1). `FL_LOG=dma` logs every DMA operation. Device 60/61 IsFencePending/IsResourcePending (never pending), context 240 SetResourceCompression (no-op).
- XG: depth targets now get the console's planes [HTILE] DEPTH [STENCIL] with xg_x's usage values (4/7/8, we had 2/3) and HTILE size; plane offsets follow xg_x. Before this the game read garbage plane sizes and DMA-filled 4 GB with 1.0f over thread stacks (heap/stack smash). TexDesc carries bind/misc now. Colour-target CMASK/FMASK planes are still missing (STUB).
- Streamed textures: uploads skip mips whose memory is reserved but not committed (`gpumem::readable`, process_vm_readv probe); this was a crash at ~131 s.
- Dev-only `FL_SKIP_MOVIES=Anthem_Tone_Video` skips the 75 s movie after the title, compiled in only with `cmake -DFL_DEV_SKIP_MOVIES=ON` (OFF by default; never in shared builds).
- VirtualAlloc logs commits/decommits/releases (`FL_LOG=memory`).

Works: boot → title (FMOD) → A → profile/offline Live → Anthem_Tone_Video (Bink) → world loads and the game renders ~870 draws a frame for minutes without crashing.
Broken: the 3D frame is black. Cause found: the game's deferred contexts (flags 0x60000/0xa0000, bit 17) are Xbox **draw bundles** that inherit render targets/viewports from the executing context (Azimuth rva 0x128ff0/0x12c540); our DXVK deferred contexts start clean, so bundle draws have no target and a 0x0 viewport. Also: DMA fills that clear GPU-owned depth placement textures only reach game memory, not DXVK; audio voices still silent (XMA/SHAPE not executed).
Next step: implement bundle inheritance. Option A: patch DXVK's deferred context (third_party/dxvk/src/d3d11/d3d11_context_def.cpp FinishCommandList / d3d11_context_imm.cpp ExecuteCommandList) so a "bundle" command list does not reset state at its start and leaves the executing context's state as the bundle ends it. Option B: record bundle calls ourselves and replay them on the immediate context. Then map DMA depth fills onto ClearDepthStencilView.

## 2026-10-26 (later): Claude: draw bundles inherit state; the 3D world renders; tonemap still outputs black

Did:
- **Draw bundles.** DXVK patch `tools/dxvk-patches/0001-xbox-draw-bundles.patch` (applied by `tools/build_dxvk.sh`, skipped when already applied): a deferred context created with flag 0x20000 records a bundle; executing its command list skips the state reset (it runs on the executing context's render targets/viewport/bindings) and restores that context's state afterwards. Lazy binding is off (`DebugLazyBinding = False`) so inherited bindings are always applied. `CreateDeferredContext` passes the Xbox bit through. Result: the main-pass bundles (~600 per frame) now draw into the immediate context's targets. The scene composite (sky, crowd, umbrellas, screens) is visible in trace dumps.
- **Still black on screen.** The tone-map pass (PS with a 16³ LUT, last full-screen draw before the copy to the back buffer) writes exactly 0 everywhere, although its scene input (the resolved HDR scene, ESRAM 0xdd0000) is bright, exposure is 1.6-7 (VS TexCoord1.w from cb1 c3/c5/c6 and the 1x1 luminance texture), the LUT and its constants (cb1 c552) are sane, and the car-layer path (t10 car depth = 0) is skipped. Not yet checked: NaN in the motion-blur branch (t3 velocity, t6 object mask, cb1 c542/c543), vignette constants c8-c13, the R8_UINT view DXVK refuses on the R11G11B10 scene (we give a zero stand-in; the composite reads it).
- Also seen: the main depth (R32, ESRAM 0x17e0000) is 1.0 everywhere at post-processing and lit geometry shows as black silhouettes: the deferred/G-buffer part of the scene is probably still missing (separate from the tonemap).
- Tools: frame trace now prints the context of each draw, constant buffers (`FL_TRACE_CB=1`, registers with `FL_TRACE_CB_REGS=3,5,6`), VS constant buffers, IA (topology/layout/vertex buffer), rasterizer + scissor, blend + sample mask, depth state, compute UAVs (dumped after dispatches), 3D-texture and small-buffer contents, depth formats with value ranges, ESRAM placement of each resource, and the targets bound at every ExecuteCommandList. `FL_LOG=shaders` logs each shader's bytecode hash (and input layouts); with `DXVK_SHADER_DUMP_PATH=research/private/shaders` DXVK writes the matching .dxbc/.spv, and `tools/spvdis.c` (links the installed SPIRV-Tools library) disassembles them. `FL_LOG=esram` logs textures created with ESRAM fields.
- Testing note: test boots now let the Anthem_Tone_Video play (the user asked to remove variables); FL_DEV_SKIP_MOVIES stays OFF in the repo.

Next step: find what zeroes the tonemap output: emulate the PS (research/private/shaders/tonemap.spvasm) on one pixel with the traced inputs, or bisect by forcing branches (e.g. a debug env that binds a known texture as t6/t3). Then the black G-buffer geometry.

## 2026-10-26 (evening): Claude: why the screen was black; past the intro into the world (with a debug override)

Found:
- The tonemap output was black because its colour matrix (cb1 c14-c16) is 0: the **camera fade** is (0,0,0,1) (ForzaMultiCam → CameraFadeComponent). The intro state flow (media/stateflow.zip, free_roam_start_ie.xml `hero_car_flow`) never reaches `fade_in`: it waits in `WaitForCutsceneLoaded` for the `jetty_transition` cutscene, whose **CinematicAudioEvent** never becomes ready: its AudioObjectOneShotFmod needs FMOD `EventInstance::getStreamLoadingState` == LOADED (3), and a DialogStatus never leaves state 0. FMOD streams on Xbox One go through the SHAPE/XMA hardware we don't emulate, so they never load. (Evidence and RVAs: FINDINGS §4.)
- With that one check forced (`FL_BREAK=0x17469a0 FL_BREAK_RETURN=0x17469a0=1`, debug only) the game plays IE_opening.bk2, fades in and reaches the festival world with the Huracán; the pause menu works (the user saw it). Lit geometry is still black (separate bug).
- Compact commands (context slot 209) are variable length: header bits 0-7 are an entry count; header & 0x0fff0000 == 0x01060000 with kind 1 binds vertex buffers (extra = stride << 48 | offset). Fixed from Azimuth's decoder (rva 0x1cbc80).
- Placement resources created twice at one address with the same descriptor now share one wrapper (the 4x MSAA depth target is created twice).

Tools: `FL_BREAK_RETURN=<rva>=<value>` (forced return), `FL_BREAK_AFTER=<s>`, `FL_BREAK_RET=<rva>` (filter by return address), `FL_BREAK_DUMP=<off>:<len>|*<off>+<off>:<len>`, `FL_BREAK_PTRVEC=<off>:<stride>`, `FL_BREAK_RDXF=1`, `FL_BREAK_DEREF` in repeat mode; `FL_DEBUG_NULL_SRV=<ps fnv>:<slots>`; trace logs ClearDepthStencilView.

Next step: make FMOD XMA streams load and play: emulate SHAPE XMA contexts (FMOD's "FMOD XMA Codec"; Azimuth: XmaVoice/XmaDecoder, FFmpeg) with an FFmpeg built with the xma2 decoder (Fedora's lacks it). Then the black lit geometry.

## 2026-10-26 (night): Claude: pause menu closes; bundle uploads, shared pages, shadow maps; the world is lit

Did:
- **Pause menu.** The game kept itself paused (States::WaitForPause) because CoreWindow never raised VisibilityChanged/Activated; Xbox ICoreWindow S+9/S+10 are add/remove_Activated (was mislabelled put_PointerCursor). Both events are raised once after activation (FINDINGS §4b). The game reaches gameplay ("DRIVE THE HURACÁN TO THE FESTIVAL!").
- **Bundles bake placement uploads in.** Uploading a placement resource while recording a bundle put a stale copy into the command list. Deferred contexts now record which placement resources they bind; ExecuteCommandList syncs them first (context.cpp, `recorded_placements`). Trees got their textures.
- **Write tracking on shared pages.** `FL_CHECK_TRACKING=1` found writes the tracker missed: re-arming one small constant buffer's 4 KiB page also cleared its neighbours' written state. placement.cpp keeps a page → resources index and marks the neighbours `force_dirty` when an upload re-arms written pages. The check now reports 0 missed writes (it reads only committed memory; it is slow, ~1 fps).
- **Shadow maps were never cleared.** The game clears its three placement shadow maps (1536² and 2×1024² D32, ESRAM-less, gpu-owned) every frame with DMA FillMemoryWithValue (HTILE 0xffffffff, depth 1.0f). Our fill only wrote game memory, so DXVK's copies stayed 0 and everything was in full shadow. dma.cpp now turns a fill of a GPU-owned depth/stencil plane into ClearDepthStencilView (planes from xg::plane_spans). Sunlight is back on trees and grass.
- **SetShaderResourceViewMinLOD** (context slot 223) is emulated with a view whose first mip is ceil(min LOD) (DXVK ignores SetResourceMinLOD). The game clamps ~1300 views (mips 1–4) while it streams the big mips. No visible change yet, but needed.
- Tools: `FL_LOG=alias` (placement resources overlapping a GPU-owned one: none found), `FL_TRACE_DUMP_DRAWS=a-b/step` (RT dumps for a draw range, `_eN` after command lists), trace logs all 128 SRV slots (materials sit in t16–t23, shared inputs up to t116), `FL_TRACE_MIPS=1` (per-mip U/Z/D state of placement textures), `FL_TRACE_PIXEL=x,y` (logs each draw that changes one RT0 pixel), `FL_LOG=lod`.

Found:
- The khaki "wall" on the right half of the screen is a world-edge mesh (main-pass draw with VB x = -8577.8, y 0..20699): visible because the near terrain is not drawn at all.
- The car layer's D16 depth (tonemap t10) is never rendered to: no draw uses it, so the car is not drawn as a car; the black shape is in the main pass.

Broken: near terrain missing; the car and pedestrians black; audio (XMA) silent, intro still needs `FL_BREAK=0x17469a0 FL_BREAK_RETURN=0x17469a0=1`.

## 2026-10-27: Claude: the world renders (bundles), SHAPE runs, the intro needs no override

Did:
- **SHAPE emulation** (src/hle/acphal/shape.cpp, FINDINGS §4): AcpHal commands 5-8/0x10/0x13 and flowgraph opcodes 1/2/0x10 (XMA -> SRC -> DMA into FMOD's rings), messages 2/4/0x10 as Azimuth posts them. XMA is decoded with FFmpeg when `tools/build_ffmpeg_xma.sh` has been run (minimal LGPL libavcodec with xma1/xma2, signature-checked 7.1.1 tarball; **not built yet: the agent's sandbox refused to compile downloaded code, the user has to run the script**); otherwise packets are consumed at their real rate as silence (STUB). FMOD's streams now load: the intro cutscene plays and ends by itself, `FL_BREAK=0x17469a0 FL_BREAK_RETURN=0x17469a0=1` is no longer needed. Note: FFmpeg-backed decoding in shape.cpp is written but has never been compiled (guarded by FL_HAVE_FFMPEG_XMA; CMake does not set it yet).
- **Bundles must not bind render targets**: track/car bundles are recorded with 1x1 dummy targets; Azimuth keeps bundle OMSetRenderTargets as shadow state only. ContextX::bundle + shadow targets. The car, roads and terrain appeared.
- **Bundle inheritance remaps** (slots 156-159) implemented in DXVK (patch 0002 + `dxvk_xbox_remap_inheritance` export); tools/build_dxvk.sh now resets the submodule's src/ and applies the whole patch series when the last patch is not in place. Materials are now correct: the gameplay frame looks like Forza Horizon 2 (Huracán, AI GT-R, road, cliffs, walls, sea).
- UpdateSubresource on CPU-written placement buffers also writes game memory (else a later re-upload restored old constants).
- Tools: FL_LOG=remap/query/create, FL_CREATE_STACK_FMT, FL_TRACE_ECL_STATS (pipeline statistics per command list), FL_TRACE_DUMP_ECLS, FL_TRACE_BUNDLES=full (bundle draw state), indirect draws in the trace, shader names: research/private/shader_names.txt maps the FNV of every DXBC in the game's zips to its file (scratch script built it).
- Testing: `FL_SKIP_MOVIES=Anthem_Tone_Video` (user's request, dev build only); gameplay starts at ~100 s / frame ~13000.

- **Driving works**: `FL_AUTOPRESS` gained RT/LT/SteerL/SteerR and `T:Name/seconds` holds; with `118:RT/60` the Huracán drives off along the coast (smoke, rewind prompt after hitting the wall) at ~58 fps. Keyboard: W/S throttle/brake, A/D steer (src/platform/input.cpp).

Broken / next: one building renders black (left of the start road); audio silent until FFmpeg is built (then compile FL_HAVE_FFMPEG_XMA and test decoding); the render thread spends ~half its time in write-tracking ioctls (gpumem::dirty / begin_upload); streaming re-reads some ranges.

## 2026-10-27 (later): Claude: shadows were black — DXVK's SRV hazard unbinding

The user saw every shadowed surface pitch black (also the "black building"). Chain: scene irradiance cube (t0, 32x32) black <- blended from probes that were black <- convolved from mip 3 of a 256x256 environment cube whose mips were never generated: the game's mip-chain CS binds the cube as a whole-resource SRV and a per-mip UAV at once, and DXVK unbinds the SRV (D3D11 hazard rules; the console has none). DXVK patch 0003 (no SRV hazards). Shadows now have sky fill; the building is lit.
Tools: FL_LOG=dispatch (every compute dispatch, any frame), FL_LOG=firstwrite (first writer of each resource), FL_TRACE_RT_MATCH=<text> (full state of draws whose RT0 matches, any frame, plus a dump of what they wrote), FL_TRACE_DUMP_SLOTS=a-b, texture dumps of arrays/cubes at the view's mip, R32G32B32A32 decoding, FL_LOG=cbwrite (16-64 byte constant buffer Map/Update contents), FL_LOG=genmips, FL_LOG=create init-data dump. Separate save dir for tests: XDG_DATA_HOME=<scratch> (runs alongside the user's instance).
Next: audio (FFmpeg build by the user), sky banding at the top of the frame, performance of write tracking.
Open (reported by the user, 2026-10-27): (1) shadows flicker at the festival; (2) crash after ~615 s: 148 "PPL task's exception was never observed" from Microsoft.Xbox.Services.dll (rva 0x17092, XSAPI tasks failing offline) in one second, then SIGSEGV in a memmove inside fl::seh::unwind_to (dispatch.cpp) while unwinding one of them. Next: reproduce both (save in research: the user's save is past the intro, at the festival).

## 2026-10-27 (evening): Claude: XSAPI rethrow crash fixed, launcher, flicker reproduced

Did:
- **Crash fixed** (the 41 s / 615 s SIGSEGVs): a catch funclet in Microsoft.Xbox.Services.dll rethrows, and our dispatcher re-caught the rethrow in the same try block, because it took the running catch's EH state from the funclet's first byte (MSVC maps funclet prologs to the try's state). This recursed until the 2 MB stack ran out; each level printed one "PPL task's exception was never observed". `ActiveCatch::entry_state` is now `try_high + 1` (cxx_eh.cpp; FINDINGS §5a). The user's old festival save, which crashed at ~41 s every time, now runs clean (4 "never observed" lines instead of 152).
- **Launcher** (launcher/forza_launcher.py, PySide6; README "Launcher"): Play/Settings/Developer/Saves/Logs pages, MangoHud and DXVK HUD settings, FL_LOG channels, test automation, save backups, live logs plus per-session log files, ninja rebuild, `--install-shortcuts` (menu + desktop icon made from the user's own WideLogo.png, never in git), `--play`, `--print-command`. The "skip long movies" switch (Anthem_Tone_Video, plus IE_opening: the ~1 min opening a fresh save plays after the logos, seen with the user's new save via /proc/<pid>/fd; skipping it is untested so far) sets FL_SKIP_MOVIES only for developer builds: it detects the "FL_SKIP_MOVIES, developer build" string in the binary. The CMake default stays OFF.
- `FL_LOG=fence`: graphics/DMA InsertFence/InsertWaitOnFence, DMA Kickoff, ExecuteCommandList and present, with thread ids.

Found (shadow flicker, open): at the festival (test save, frames 17500-17504) consecutive frames alternate between shadows and no shadows (car and barrier shadows missing in 17500/17502, present in 17501/17504). The user says it starts after driving through shadowed areas. The shadow-map clears (DMA FillMemoryWithValue -> ClearDepthStencilView) run on the immediate context when the game calls them, while the console's DMA engine first waits on a graphics fence (dma InsertWaitOnFence is a no-op), so a clear can land between the shadow pass and the lighting pass that samples it. Not confirmed yet. A first FL_LOG=fence,dma trace up to frame 9700 (still loading) showed DMA depth clears only before 36 s.

Next: trace FL_LOG=fence,dma around frame 17500 (test save, festival) to see which thread issues the shadow clears and where they fall relative to the ECLs and presents of the same frame. Then order DMA work after the graphics fence it waits on.
Note for testing: don't start a test instance while the user is playing; both share the GPU.

## 2026-10-27 (night): Claude: frame timing, stability

Did:
- **Frame pacing** (dxgi.cpp `pace_present`): presents wait on a virtual 60 Hz vblank clock (`sync` x 1/60 s), so interval 2 = 30 fps on any monitor. The game's update steps a fixed 1/29.97 s per frame, so unpaced on the user's 260 Hz monitor it ran ~4x too fast. The user confirms the speed is right now, but it feels choppy. `FL_FRAME_PACING=console|dynamic|off`, `FL_FPS_CAP`. The launcher has a Frame rate setting.
- **Dynamic FPS: investigated, parked at the user's request.** FINDINGS §6c has the update-step function (rva 0xf58770) and the engine options (`synchroniseUpdateToRenderThread`, `unlockframerate`, `lockupdatetimeto60fps`). The retail game doesn't read them from argv. The runtime now passes `-- <game options>` through `__getmainargs` anyway; `FL_FRAME_PACING=dynamic` logs UNIMPLEMENTED because it still runs too fast. Next step: find how options reach rva 0x299950 (CForzaCommandLineParameters), or patch the `je` at rva 0xf587dd ourselves.
- **Stutter measurement:** `FL_LOG=fps` logs, every 10 s, average fps, 1% low, worst frame, frames over 50 ms, frame-time p10/p50/p90, and placement-sync counters (syncs, ms spent scanning, uploads in MB and ms). `FL_TRACE_TIME=1` histograms QueryPerformanceCounter callers and counts WaitForVBlank calls (none).
- **Texture untiling ~8x faster** (gcn.cpp `untile_fast`): one AddrLib call per element took ~13 ms for a 2048² BC1 texture on the render thread, during streaming hitches. Now each tile's base plus a repeating offset pattern (XOR model with AddrLib's 64x32 block, additive with 256x64, 8x8 for 1D), checked against AddrLib at 256 points per call, with fallback to the exact path. tests/gcn_layout_test compares it with the per-element reference on 8 layouts (all pass, byte-exact).
- Presents after pacing use interval 0 (mailbox, no tearing in a compositor). A FIFO present blocked up to a refresh plus the frame queue, and steady driving showed 39/28 ms frame pairs (1% low a constant 38.9 ms). `FL_VSYNC=1` brings FIFO back.
- `FL_FULLSCREEN=1` and F11 / Alt+Enter (borderless fullscreen; launcher toggle). The refresh rate is logged at start, with a warning when it isn't a multiple of ~30 Hz: 30 fps on 144/260 Hz judders by nature, while 60/120/240 Hz or variable refresh is smooth. The user's DP-2 has Vrr: Never.
- Map with DO_NOT_WAIT returning WAS_STILL_DRAWING is no longer logged as an error (the game polls read-backs).

- **Placement write tracking per frame instead of per bind** (placement.cpp "frame scan"). FL_LOG=fps showed ~356 command lists per frame, each re-syncing its placements, plus ~6800 bind syncs: ~15000 PAGEMAP_SCANs (~10 per resource), ~4.7 ms of each frame. Now each Present re-arms a quarter of all tracked memory in one non-strict PAGEMAP_SCAN; the kernel skips untracked memory (pagemap_scan_test_walk: with PM_SCAN_WP_MATCHING but without PM_SCAN_CHECK_WPASYNC, VMAs without async WP are skipped). Every resource on a written page is marked dirty and "hot". Resources not written for 10 frames ("cold": ~95% of syncs) skip the per-bind scan; hot ones keep it. Writes to cold resources reach the GPU at most 4 frames late. Cost is ~1.4 ms per frame while streaming and less when steady (was 4.7). `FL_NO_FRAME_SCAN=1` brings back the old scheme. Screenshots with and without it match.
- The virtual vblank is 59.94 Hz (16.683 ms): the game steps 1/29.97 s. A frame more than 4 ms late restarts the cadence instead of bursting the following frames.
- FL_LOG=fps measures at hand-off to DXVK (after pacing). The earlier numbers timed the game's Present calls, which jitter with frame work. Steady driving now hands off 300/300 frames at 33.4 ms. The rest of the visible judder is the 260 Hz display (8.68 refreshes per frame); 240 Hz or VRR fixes it.

Measured (10 min soak, 30 fps pacing, driving, before the frame scan): no crash; menus a steady 30.0 fps; world load at 50-70 s had 0.5-1.5 s frames; driving into new areas had 120-330 ms hitches. With fast untiling: worst 57-72 ms while driving, world load worst 431 ms.

- Untile plans are cached per layout (addrlib inputs + slice). The tile bases and in-tile pattern are checked against addrlib once (every element for small levels, 1024 samples + corners otherwise); after that a level needs no addrlib calls. With a warm cache that's 10-50x faster than per-element addrlib (2048² BC1 mip chain 0.48 ms vs 12.9 ms). World load (1.4 GB of uploads): untile 628 → 318 ms, worst frame 389 → 78 ms.
- Levels of 128k+ elements are untiled in row bands on a small host worker pool (common/workers.h `parallel_for`, 2-8 threads that never run game code): 2048² BC1 mip chain 0.48 → 0.23 ms.
- Soaks (10 min each, 30 fps pacing): test save driving, worst frame 33.4 ms outside the world load except two frames (55, 48 ms) while streaming 430 MB; the user's old festival save, worst 33.6 ms; RSS flat at 2.9 GB, 135 threads; no crash.

- Freeze watchdog (crash.cpp): no Present for 15 s → `watchdog: no Present for N s` plus a thread dump in the log, once per freeze; `FL_WATCHDOG=0` turns it off. Freeze reports from the user now carry where every thread was.
- A 25 min soak (driving bursts every minute, 30 fps): no crash. 130 ten-second windows, 5 with a frame over 34 ms (3 of them at the world load). RSS 3.84 GB at 84 s, 4.16 at 678 s, 4.19 at 1305 s (streaming fills the game's pools; levelling off).

- **A game thread spun a whole core** at 30 fps: the game's lightweight semaphore (rva 0x1088930: lock xadd a token, put it back, check the timeout via the clock at rva 0x1083050, SwitchToThread, retry) waits by yielding. On the console that thread owns a core; here sched_yield returns at once. SwitchToThread / Sleep(0) now back off: after 200 back-to-back yields, ~0.1 ms sleeps. Process CPU at 30 fps went from ~470% to ~287% of a core, and pacing is unchanged (300/300 frames at 33.4 ms). Thread dumps (FL_DUMP_THREADS_AT) now include each thread's CPU time, so two dumps show who burns CPU.
- Fullscreen (FL_FULLSCREEN=1) checked on the real display: borderless 1920x1080, renders correctly. Saves were already crash-safe (ConnectedStorage writes `<container>.new`, fsyncs, renames, recovers `.old`).
- Monkey runs (random buttons every 1.3-1.5 s for 8 min, two seeds): pause menu, rewind, replay camera, cockpit view, "Feature Locked" dialog; no crash. New UNIMPLEMENTED seen: WinRT Windows.Xbox.ApplicationModel.Help (the game copes).

Next: the shadow flicker (back burner per the user), dynamic FPS (§6c), audio (the user runs tools/build_ffmpeg_xma.sh).

## 2026-10-27 (morning): Claude: audio plays (XMA via FFmpeg), crackle found and mostly fixed

- The user built `tools/build_ffmpeg_xma.sh` (the agent may not compile downloaded code). The script now passes --disable-x86asm (no nasm here), is executable, and applies tools/ffmpeg-xma-patches/*.patch. CMake picks the decoders up on reconfigure (`cmake -S . -B build`).
- First sound was "super crackly". Measured on recordings of the sink monitor (parecord, second-derivative click detector) and with FL_LOG=audio counters (SHAPE graph rate, starved voice-frames, DMA overwrites/over-reads, WASAPI low-queue checks, per-voice SRC steps vs graphs, stop reasons). Three causes, in order found: the WASAPI buffer was smaller than the device period (fixed: 4 periods); FFmpeg's XMA hold-back (patch written, **user must re-run tools/build_ffmpeg_xma.sh**); and the main one, graphs run on our own clock instead of on submit (fixed). FINDINGS §4 has the details.
- Diagnostics: FL_DUMP_XMA=<dir> dumps the first 40 voices' packets and decoded PCM; FL_LOG=audio has the counters above. One intermittent startup crash inside DXVK (rip libdxvk_d3d11+0x1f6fa9, 0.28 s, radv in the stack) did not repeat; its log is in the session scratchpad.

Next: rebuild FFmpeg with the patch and re-measure (expect fewer clicks in gameplay: sound tails); replace the linear SRC interpolation with a windowed-sinc resampler like the hardware's (Azimuth: up to 40 taps); start voices whose read offset is inside a packet at the right frame (now logged as "XMA read starts inside a packet").

## 2026-10-27 (late morning): Claude: car shadow flicker fixed (bundle state)

- Symptom: the car's shadow vanished every 4th frame at some spots. 64 consecutive screenshots showed the exact period 4; the bad frame is always the one that re-renders the 2048² static cascade.
- Ruled out by measurement: late CPU writes after our uploads (FL_LOG=latewrite now compares the bytes of every small uploaded buffer at Present), missed write tracking (FL_CHECK_TRACKING=1/buf now reports with the epoch and cold/scanned, and FL_LATE_FROM=<frame> limits it to frames of interest), the frame-scan slicing (FL_SCAN_PARTS=1 still flickered), DMA clear ordering (all on the render thread, in order).
- Cause and fix: FINDINGS "Bundle sets must never be filtered as redundant". DXVK patch 0004; `tools/build_dxvk.sh` applies it. Also: a bundle's first draw always re-applies its topology (context.cpp).
- New trace aids: FL_TRACE_ECL_STATE=1, CS SRV binds and ClearState in traced frames, the ECL restore flag.
- Testing note: the game autosaves during test runs, so the start position drifts; keep a copy of the test save and restore it before each run.

Next: confirm in play at the spots where the user saw it; then streaming hitches and the DXVK startup crash.

## 2026-10-27 (midday): Claude: the car no longer motion-blurs

- Symptom (Monty's save, donuts at the festival): the car blurred with the background; on PC ports it stays sharp. Cause and fixes: FINDINGS "Motion-blur car mask". Verified on 8 donut screenshots: car sharp, background blurred; mask dump has stencil 2 on 360k pixels (was all 0).
- Tools: FL_LOG=shaders now prints the DXBC checksum (= DXVK_SHADER_DUMP_PATH file name), FL_LOG=create logs CreateBuffer with its caller, traced CSSetShaderResources print the game caller, FL_TRACE_DUMP decodes R8 (with a value histogram) and R16G16_FLOAT, DSS dumps show the stencil masks and ops. FL_TRACE_DUMP no longer races (per-thread state; deferred contexts are not dumped).

Next: confirm in play; check other passes that read stencil views (they now see stencil in R as well as G).

## 2026-10-27 (afternoon): Claude: MSAA works (FMASK stand-in)

- Monty: "the edges of models are very pixelated". Cause and fix: FINDINGS "MSAA resolve needs FMASK". A/B on Monty's save at frame 1500 (FL_FMASK_ZERO=1 vs default): jagged car and checkerboarded grass/trees vs smooth.
- Note: a test instance started while Monty plays receives their controller input too.

Next: check the cost of the full 4-sample resolve (FL_LOG=fps) and other users of the FMASK view, if any.

## 2026-10-27 (afternoon): Claude: display modes

- FL_DISPLAY_MODE=windowed|borderless|fullscreen (window.cpp; FL_FULLSCREEN=1 still means borderless). Borderless = a borderless window over the display at its desktop mode; fullscreen = an SDL exclusive display mode, the closest to FL_WINDOW if set, else the desktop mode (Wayland: a smaller mode is scaled; the output's own mode and refresh stay). Both start on the primary display (a new window otherwise goes to the monitor KWin picks). F11/Alt+Enter switches between windowed and the chosen mode.
- Launcher: "Display mode" combo (Windowed / Borderless window / Fullscreen) replaces the fullscreen toggle; an old `"fullscreen": true` config becomes borderless.
- Checked: each mode on Monty's two-monitor KDE Wayland desktop (spectacle screenshots: windowed window; borderless and both fullscreen sizes fill DP-2 and render; logs "display mode: fullscreen 1920x1080 @ 260.00 Hz" / "1280x720"). Not checked: the F11 toggle (no way to send keys from the agent).

## 2026-10-27 (afternoon): Claude: dynamic frame rate at normal game speed

- FL_FRAME_PACING=dynamic now works (FINDINGS §6c "Dynamic frame rate works"): a checked patch of the update-step branch (src/loader/game_patches.cpp), paced to FL_FPS_CAP or the display's refresh rate. Verified by timed acceleration at ~95 fps, 60 fps and console 30 fps.
- Launcher: Frame rate "Dynamic: any frame rate, normal game speed" and an FPS cap preset list (monitor refresh, 60, 90, 120, 144, 165, 240); the judder hint knows the cap.
- Test aids: FL_SCREENSHOT_AT_TIME=<s,...> (screenshots/time_<s>.png), FL_BREAK_XMM=1 (float arguments at breakpoints); scripted runs (FL_AUTOPRESS) ignore the real controller and keyboard and don't rumble it (FL_AUTOPRESS_LIVE=1 keeps them), so a test started while Monty plays isn't driven by his pad.
- Not done: 120 fps in the open world (GPU-bound ~90 fps with the full 4x MSAA resolve): optimisation later, as Monty asked.

## 2026-10-27 (afternoon): Claude: repo renamed to Open Horizon 2

- GitHub repo MontyMcK/forza-linux renamed to **MontyMcK/open-horizon-2** at Monty's request (a proprietary project has a similar name); GitHub redirects the old URL. README title updated; the binary, the launcher and the FL_* variables keep the forza-linux name for now.

## 2026-10-27 (afternoon/evening): Claude: performance pass, ~90 -> ~175 fps in the open world

- Monty: "I have proper horsepower and I'm not getting crazy frames". Measured in dynamic mode (his request: test at max fps) on his save while driving. Causes and fixes in FINDINGS "Performance: bundles and DXVK's flush/restore logic": DXVK patches 0007 (bundle chunks don't trigger flushes) and 0008 (restore only what a bundle bound), the DMA fill lookup, plus the upload/VirtualFree race fix.
- New tools: FL_GPU_PROFILE(_AT_TIME) (src/hle/d3d11x/gpu_profile.cpp), FL_CPU_PROFILE (src/loader/cpu_profile.cpp), DXVK counters in FL_LOG=fps (patch 0006).
- Results: 5 min soak, dynamic (refresh cap 260 Hz): 153-193 fps average, 1% low ~80-90 fps, no crash; screenshots correct. Its one 85-117 ms frame every 30 s was the soak's own FL_SCREENSHOT_AT_TIME capture (read-back + PNG on the render thread): a 3 min run without screenshots had no frame over 20 ms (FL_LOG=fps now logs every frame over 40 ms as "hitch"; FL_LOG=file times FlushFileBuffers). Autosaves (every 60 s) cause no hitch.
- Next: the render thread is still the limit (~5 ms/frame): ~10k CS chunks per frame from bundle remaps (AllocCsChunk, CS thread at ~50%), the page-tracking ioctls (~17% of the render thread: per-bind scans of hot resources and the per-frame scan); frame times are bimodal (p10 3.8 ms / p90 ~9 ms), probably the 4-frame static-cascade cycle.

## 2026-10-27 (evening): Claude: two of Monty's bugs from play

- Crash at 66 s: "DXVK returned object ... that has no wrapper" from OMGetDepthStencilState. Cause: duplicate state objects (FINDINGS). Fixed; the reuse path is hit in normal play (logged once), and his 23 min session after the fix had no crash.
- "Got to the end of a mission and it didn't register": the game asks CoreApplication for a GPU-policy interface at cinematics and event ends and threw when it was missing (FINDINGS "CoreApplication GPU policy"). Implemented; the calls now succeed (put 0, put 1 at the world's opening). Not re-tested at a mission end yet: Monty to confirm.
- Also: XAudio2 is looked up at each cinematic (LoadLibrary xaudio2_9.dll, GetProcAddress CreateXAudio2Object -> null); the game copes, but cinematics may be silent. Check.

## 2026-10-28: Claude: Monty's 84-minute crash (heap corruption from PPL token registrations)

- Crash: `free(): chunks in smallbin corrupted` while the game released a render target's views (glibc only noticed it there). The core (coredumpctl, glibc debuginfo via debuginfod) showed the first chunk in the 0x50 smallbin with its bk one too low, and its stale bytes were one of our ConcRT `Registration`s (token, state 2, callback 0x180017600 in Microsoft.Xbox.Services.dll): a refcount decrement on a freed registration.
- Cause and fix in FINDINGS 5a "Cancellation registrations hold initialRefs + 1 references": token_register now gives the list its own reference, reg_invoke always drops it (like msvcr110 `_Invoke`), deregister skips a callback a cancel hasn't started yet, and the task collection's internal registration passes 0 (only the list holds it).
- Checked: tests/concrt_token_test.cpp passes (6 of its 7 checks fail against the old code); a 170 s run on Monty's save with FL_LOG=concrt: 12 registrations, all 12 destroyed by the Services DLL's own release after `_DeregisterCallback` (0x18002620c, 0x180105f9c, 0x18002078c), game in the world as normal.
- Not proven by a long run yet: the crash needed ~340 bad writes to land on a heap link, so only a long session will show it's gone. Same class of bug to look for: any HLE `_RefCounter` or COM object whose count disagrees with what inlined game code releases.

## 2026-10-28 (evening): Claude: buying a car crashed (WIC PNG thumbnails)

- Monty: "buying a car crashed my game". FATAL `UNIMPLEMENTED combase!CreateStreamOnHGlobal` from rva 0x113018d: the garage-thumbnail PNG encode (FINDINGS 4h). Failing the call cleanly instead left the purchase stuck on "Saving..." (the job returns buffer -1), so the fix is a minimal WIC in src/hle/wic/wic.cpp: memory IStream (CreateStreamOnHGlobal), `WindowsCodecs.dll!WICCreateImagingFactory_Proxy` (LoadLibrary now treats WindowsCodecs.dll as an HLE module), PNG encoder/frame, CreateBitmapFromMemory and a format converter for 32bppRGBA/BGRA/BGR, using common/png's new `encode_png`. Every other factory slot logs `UNIMPLEMENTED IWICImagingFactory slot N` and returns an error (the game coped with no factory before); other interfaces' unknown slots trap.
- Checked: tests/wic_png_test.cpp (the game's call sequence, both paths, PNG decoded back) passes. Scripted purchases on copies of Monty's save (Menu, RB RB, A Buy Cars, Right x3 to the Audi R8, A, A, A Buy): both PNGs encoded and saved (bigThumb.png is a correct render of the car), "Saving..." clears, "Exit Autoshow" -> back to the menu with 150,000 CR taken and the R8 in the featured cars. Monty's own crashed purchase (McLaren P1) is in his garage and shows a thumbnail in My Cars.
- Seen at the same moment, not fixed: `D3DMapEsramMemory: mprotect(..., 53 pages) failed` (twice per thumbnail render, two threads); the thumbnail is still correct. Check errno/alignment there next.
- Still unimplemented imports (FL_LOG=missing): vccorlib `Platform::Details::CreateValue`, MFPlat `MFCreateMediaType` / `MFInitMediaTypeFromWaveFormatEx`, MFReadWrite `MFCreateSourceReaderFromURL`.
- Also this evening: Monty's save edited to 999,999,999 CR (credits are a 32-bit int in ProfileData: entry `[u32 len][name][0x20][u32 0][u32 type 3][i32]`, "ssmc" header = magic + payload length, no checksum); backup in ~/.local/share/forza-linux-backups/.

## 2026-10-30: Claude: the project is GPL-3.0-or-later

- Monty: "give the project a GPL-3 licence". LICENSE (the GNU GPL v3 text), a README "License" section (or-later notice, third-party licenses, the game not covered), the PLAN decision and a CLAUDE.md Rule 1 note (copy only GPL-3-compatible code). Same change as on the ps5-test branch.

## 2026-10-30 (evening): Claude: PS5 port, Steps 1-3 (setup, research, payload probe)

- Monty asked for a PS5 port (runtime + native launcher as one homebrew title). Plan, checklist, pinned dependencies and verified-vs-trusted facts: docs/PS5_PORT.md. Work is on branch `ps5-test` in the worktree `~/Projects/open-horizon-2-ps5` (made from `main`; the main checkout's uncommitted WIC work on `test` was left alone). `build-dxvk`/`build-ffmpeg-xma` there are symlinks to the main checkout's builds (excluded in .git/info/exclude).
- Setup: `tools/ps5/` scripts + gitignored `tools/ps5/ps5.env` (console IP, ftpsrv :2121, pldmgr :8084). elfldr v0.27 installed into pldmgr at Monty's request (not autoloaded; send_payload.sh starts it). FTP round trip checked. Title ID PPSA99642. Logs: files pulled over FTP into research/private/ps5-logs/.
- Monty's dump: Mount/ copied to Monty's 500 GB USB stick (exFAT, MBR) to go into the PS5; the stick writes at ~6 MB/s.
- Rule 0: `tools/check_no_game_data.sh --package DIR` (tested: blocks a dump file by name+size and a renamed one by bytes); ps5ftp.py refuses game data and writes outside the title folder.
- Probe (tools/ps5/probe/oh2probe.c) as a payload: 24 pass, 2 fail. Results in FINDINGS §7: exec via sceKernelMprotect, GS base via sysarch, umtx, 16 KiB pages, reservations at 0x140000000 and 0x1000000000, mcontext at +64 and FXSAVE at +320, SIGSEGV write faults at ~41 us. Fails: MAP_FIXED|MAP_EXCL overwrites (as reported), 1 GiB direct memory refused in a payload (no budget). The console reports firmware 13.590.001 (Monty: 13.20).
- Linux check: gcn_layout_test and dxvk_smoke pass, FL_EXIT_AFTER_FRAME=300 exits cleanly.

- Step 4 started: src/platform/os/ (README status table, FL_OS backend selection in CMake), futex and file existence moved, tests/os_test.cpp passes; Linux check passes.

Next: Step 4's memory area (reserve/commit/protect/exec/page size: src/hle/kernelx/memory.cpp, src/loader/pe_image.cpp, the d3d11x gpumem/placement mappings) behind src/platform/os/memory.h, then the rest of the Step 4 list. In parallel, Step 3b (the probe as a title; needs Step 5's packaging) settles direct memory and whether a title can read /mnt/usbN. The USB copy of Monty's dump was still running at the end of this entry: verify every file's size against game_data/ before telling Monty to unplug it.

## 2026-10-30 (late evening): Claude: GPL-3.0-or-later licence; dump goes to internal storage

- Monty: "give the project a GPL-3 licence". LICENSE (the GNU GPL v3 text, md5 1ebbd3e3...0464, as shipped by the PS5 SDK and PS5_Vulkan), README "License" section (or-later notice, third-party licences, game not covered), PLAN decision, CLAUDE.md Rule 1 note (copy only GPL-3-compatible code). Committed on ps5-test only: main still has no LICENSE until Monty says to add it there (the PS5 brief says never touch main).
- Monty will copy the dump from the USB stick to the PS5's internal storage, and starts elfldr after each jailbreak. ps5.env and PS5_PORT.md updated; the dump path on the console is still to be confirmed with Monty.

Next: unchanged (Step 4 memory area; Step 3b title probe).

## 2026-10-30 (night): Claude: dump on the console, title toolchain, Step 4 complete

- Monty's dump: copied on the console from the USB stick to `/data/OpenHorizon2/Anthem_1.0.0.64_x64__8wekyb3d8bbwe/Mount` with our `oh2copydump` payload (Monty asked; 1474 s, 27 MiB/s); every file's size matches the PC copy, checked on the console and again over FTP.
- Title toolchain (Step 5 start): `tools/ps5/setup_deps.sh` (pinned PS5_Vulkan f410e67 + its SDK fork b83202b and PS5_Mesa 1ba9874 in build-ps5/deps), `cc.sh`, `link_title.sh`, `package.sh` (stage, audit, build), `ps5/title/sce_sys` (PPSA99642 param.json, original icon from `make_icon.sh`). The probe builds as a title (`build.sh probe-title`); `build.sh launch` is a payload that starts the title. PS5_Vulkan's own skeleton build at f410e67 misses libps5platform.a (ps5_fp_ieee undefined); our link adds it.
- Uploaded the probe title: ShadowMount+ found "Open Horizon 2" but registration failed ("AppInstallAll hook is no longer installed / TitleDir bridge unavailable", gave up after 2 retries), the same as on 2026-10-04. Fix: a full PS5 restart + jailbreak + elfldr (asked Monty). The FTP outage at 19:46 was not a reboot (SM+ kept its 17:02 session).
- Step 4 complete: src/platform/os/ (futex, files, memory incl. checked copies, thread env, signal context, audio, window, input/controller output, write watch, diagnostics) with the Linux backend as the `fl_os` library; nothing outside os/linux uses SDL, /proc, <linux/...>, raw syscall() or the ucontext layout. tests/os_test.cpp (24 checks) is the backend contract. Every slice passed the Linux check (tests + frame 1500; FL_LOG=fps, FL_CPU_PROFILE and FL_DUMP_THREADS_AT where they exercise the moved code). Behaviour note: VirtualAlloc at a fixed address no longer over-maps 64 KiB past the end (it failed if that extra space was taken).
- Started: PS5_Vulkan's RADV release build (build-ps5/radv-release.log; mako/markupsafe in build-ps5/deps/pymods via PS5VK_MAKO_PATH).

Next: after Monty's restart, check SM+ registered PPSA99642 (/data/shadowmount/debug.log), then `tools/ps5/send_payload.sh build-ps5/launch/oh2launch.elf` and pull `probe-title.txt` (logs folder or /download0) for Step 3b. Then Step 5: cmake/ps5.cmake, static DXVK with a VK_KHR_display WSI, FFmpeg XMA, addrlib; Step 6: the PS5 backend of src/platform/os/ (must pass os_test on the console).
- RADV release build (`PYTHONPATH=build-ps5/deps/pymods PS5VK_MAKO_PATH=build-ps5/deps/pymods bash tools/build-radv.sh release` in build-ps5/deps/PS5_Vulkan) stops at Mesa's host CLC step: "Dependency LLVMSPIRVLib not found". Needs (sudo, asked Monty): `sudo dnf install spirv-llvm-translator-devel libclc-devel spirv-headers-devel python3-ply clang-devel`. Then rerun the same command (log: build-ps5/radv-release.log).
- `cmake/ps5.cmake` added (wraps the SDK fork's prospero.cmake, znver2, no host pkg-config, FL_OS=ps5).

## 2026-10-30 (late night): Claude: title probe results, NAS handoff, PS5 OS backend passes on the console

- Monty restarted the PS5 twice; the second restart was needed because our first redeploy deleted eboot.bin before renaming the new one in, and ShadowMount+ gave up on the title (fixed: atomic rename, eboot/param.json last).
- Title probe (FINDINGS 7.5): a title sees only /app0 and /download0, so the dump in /data/OpenHorizon2 is out of reach (decision needed: newer ShadowMount+ that mounts /data into apps, or the Lapy daemon, which isn't on the allowed list). 12 GiB direct memory, 8 GiB in one allocation; libc malloc 13 MiB; 250 open files. Logs come over TCP to tools/ps5/netlog.py (loopback FTP from a title fails).
- NAS handoff (Monty asked): docs/NEW_MACHINE.md, tools/nas_sync.sh (bundle, research/private, agent notes, ps5.env, START HERE.md) run by tools/git-hooks/pre-push in the background; skill .claude/skills/nas-handoff.
- Step 6: src/platform/os/ps5/ written; tests/os_test.cpp 22/22 on the console as the title (build.sh os-test-title). link_title.sh now uses PS5_Vulkan's radv_link_recipe (unwind linker script, libc++, builtins via a clang shim for Fedora's compiler-rt path, platform heap and libc wraps). Linux: the crash handler calls os::write_watch_on_fault and ReadFile calls os::write_watch_before_kernel_write (no-ops on Linux); Linux check passes.

Next: Monty's storage decision; RADV release build once the Mesa host packages are installed (LOG above); then the runtime built for the PS5 (CMake + link_title.sh) for M1, DXVK static with a VK_KHR_display WSI for M2.
- ShadowMount+ updated on Monty's request: 1.7beta11 (`~/Downloads/shadowmountplus.elf`, sha256 08c84301...f3da = the release's) uploaded to `/data/pldmgr/payloads/shadowmountplus/shadowmountplus.elf`; the old 1.7beta2 is `shadowmountplus.elf.bak` there. Takes effect at the next restart + jailbreak; a background watcher then reruns the title probe to check /data and USB access.
- **First PS5 compile of the runtime (syntax only):** 25 of 82 sources compile; 55 fail on "'ms_abi' calling convention is not supported for this target" (clang's x86_64-sie-ps5 target rejects it; our whole HLE uses WINAPI = ms_abi), 2 on a missing zlib.h (PS5 zlib: PS5_Vulkan builds one in .deps/native/zlib). Next: compile the runtime for a target that accepts ms_abi with the same SysV ABI (e.g. -target x86_64-unknown-freebsd with the SDK sysroot and the PS5 flags from tooling/prospero-clang18), then link with link_title.sh; or check clang's PS4/PS5 calling-convention check.
- ShadowMount+ 1.7beta11 verified: the title reads the dump on /data/OpenHorizon2 and USB (FINDINGS 7.5). Test titles must not return from main (Monty saw a failure each time); they now idle until closed. Monty confirmed: the title shows a black screen, no error.
- ms_abi on the PS5: compile the runtime with `-target x86_64-unknown-freebsd9` + `-fshort-wchar -D__PROSPERO__=1 -D__SCE__=1` + the PS5 flags against the SDK sysroot (FINDINGS 7.6); a test object links into a title.

Next: make cmake/ps5.cmake build the runtime that way (host clang, freebsd9 target, SDK sysroot + libc++ headers, the flags in FINDINGS 7.6), add a PS5 branch to CMakeLists.txt (FL_OS=ps5: no SDL/pkg-config, DXVK/FFmpeg/zlib for the PS5 later, link with tools/ps5/link_title.sh), and get forza-linux to compile; then M1 (load the exe from /data/OpenHorizon2 via --game-dir, resolve imports, log over netlog).

## 2026-10-30 (late night): Claude: M1 on the console

- RADV release archive built (build-ps5/deps/PS5_Vulkan/.deps/native/radv-release, Mesa 1ba9874) after Monty installed the Mesa host packages (+ spirv-tools-devel).
- The runtime compiles for the PS5 with tools/ps5/cc-win.sh (FINDINGS 7.6) and links into the title (`build.sh runtime`); `run_title.sh` starts it with `--game-dir $PS5_DUMP_REMOTE` and `--env FL_LOG_FILE=...` (new `--env` option in main.cpp, harmless on Linux).
- **M1 done** (FINDINGS 7.7): exe loaded from /data/OpenHorizon2, imports resolved, log pulled over FTP. The game then needs its fixed-address title memory (0x60000000000 etc.), which a title can't reserve, and faults.
- Portability fixes along the way: FindFirstFile's own wildcard matcher (Linux check: same archives found), os::current_stack, os::random_bytes, qsort without qsort_r, no std::atomic_ref, bind_import, PS5 stand-ins for link/symlink/readlink/pathconf/timegm/strsignal.

Next: Xbox title-memory addresses in the PS5 memory backend (remap the fixed reservations into ranges a title can reserve, e.g. 0x1000000000 upward, and teach VirtualAlloc to place them; check with the decomp how the game uses those addresses (FINDINGS 6, Azimuth's layout)); then DXVK static with a VK_KHR_display WSI on RADV for M2.
- Past M1 (FINDINGS 7.7): Monty's photos showed SIGSYS from usage()'s exit() (titles can't exit: os/process.h, saved launch args) and the known SIGSEGV. The SIGSEGV was the failed title-memory reservations (garbage count, wrapped HeapAlloc size), fixed by placing unavailable VirtualAlloc ranges elsewhere and keeping PS5 address-less reservations above 64 GiB. The runtime now runs the game's start-up into the game loop and stops at D3D11CreateDevice (no PS5 DXVK yet). Game heap: src/hle/game_heap (large blocks on their own pages). Monty is fine with test launches (each failed run shows an error dialog on the TV); crash details come from our log, photos only for crashes before logging starts.

Next: M2. Build DXVK for the PS5 (static, against PS5_Vulkan's RADV archive, with tools/dxvk-patches), add a VK_KHR_display WSI for VideoOut, replace tools/ps5/m1/no_dxvk.cpp, and link RADV via RADV_ARCHIVE in link_title.sh (plus the AGC stub .so files rpcs3-PS5's link-title.sh builds).
- M2 plan (DXVK on the PS5), started 2026-10-30 night: (1) tools/ps5/build_dxvk.sh: export DXVK v3.1.1 (the main checkout's third_party/dxvk submodule, `git archive`) into build-ps5/dxvk-src, apply tools/dxvk-patches then PS5 patches (tools/ps5/dxvk-patches/), meson cross build with the SDK fork's prospero-clang (PS5_Vulkan's tooling/radv/ps5-cross.ini as the model), static d3d11/dxgi/dxvk libraries. (2) PS5 patches: the Vulkan loader takes RADV's `vk_icdGetInstanceProcAddr` directly (no dlopen in a title; the RADV archive exports it); a `ps5` WSI backend modelled on src/wsi/sdl3 (~420 lines) presenting through VK_KHR_display (one display, its mode as the window size). (3) link_title.sh: RADV_ARCHIVE + AGC stub .so files (as rpcs3-PS5's link-title.sh), drop tools/ps5/m1/no_dxvk.cpp. Monty said "go" to DXVK first (a status notification was offered, not needed).
- tools/ps5/build_dxvk.sh written (copies the main checkout's patched DXVK tree to build-ps5/dxvk-src, applies tools/ps5/dxvk-patches/*.patch, meson cross build with the SDK fork's prospero-clang, `-DDXVK_PS5=1`). First configure stops at meson.build:165 "SDL3, SDL2, or GLFW are required to build dxvk-native". The PS5 patch to write (tools/ps5/dxvk-patches/0001-ps5-wsi.patch): meson_options.txt `native_ps5` feature; meson.build: if enabled add `-DDXVK_WSI_PS5` and skip the SDL/GLFW error; src/wsi/meson.build: ps5/*.cpp; src/wsi/wsi_platform.cpp: register a `Ps5WSI` WsiBootstrap (like Sdl3WSI in sdl3/wsi_platform_sdl3.cpp); src/wsi/ps5/: driver with getInstanceExtensions = VK_KHR_surface + VK_KHR_display, one monitor (the display's mode from vkGetPhysicalDeviceDisplayPropertiesKHR, or 1920x1080), window size = that mode, createSurface via vkGetDisplayPlanePropertiesKHR + vkCreateDisplayPlaneSurfaceKHR; window/fullscreen calls succeed trivially. Then 0002: src/vulkan/vulkan_loader.cpp loadVulkanLibrary() returns RADV's `vk_icdGetInstanceProcAddr` on DXVK_PS5 instead of dlopen. Then static d3d11/dxgi (they're shared_library() in DXVK's meson).

## 2026-10-30 (very late): Claude: HANDOFF, M2 in progress (DXVK creates its device on the PS5)

Where things stand (Monty asked for a handoff):
- **Done:** M1. The runtime runs as title PPSA99642 from /data/OpenHorizon2 (ShadowMount+ 1.7beta11), through the game's start-up into its game loop. **DXVK is ported and linked with RADV, and creates a D3D11 device on the PS5 GPU** (FINDINGS 7.8). Linux build checked after every change (latest: tests pass, frame 600 exits cleanly).
- **Blocker now:** direct memory is exhausted right after DXVK's device creation ("os: commit 0x917440000+0x10000: no direct memory"); the game then faults on a null allocation. The commit error now logs our committed total and the largest free block: rebuild is done (`tools/ps5/build.sh runtime`), not yet uploaded or run because the console's FTP stopped answering (asleep/rebooted: Monty re-runs the jailbreak, then elfldr).
- **Likely next fixes:** (1) see from that log whether RADV grabbed most of the 12 GiB (then cap its heaps, e.g. a PS5_Vulkan/RADV env or option for the pool size, or give it the Xbox-like 5 GiB title budget split) or whether our commits are large (the game reserves 4+4+4 GiB title regions and commits inside; check the sum; Windows commit charge vs. our 64 KiB-unit backing; decommit doesn't return memory yet, src/platform/os/ps5/memory.cpp). (2) DXGI EnumOutputs returns nothing ("monitors not associated with any adapter"): the PS5 WSI monitor needs an adapter LUID match for the swap chain/fullscreen path. (3) Then the swap chain on VK_KHR_display and the first frame: M2 TEST CARD.
- **How to run it:** `tools/ps5/build_dxvk.sh` (only after changing tools/ps5/dxvk-patches or tools/ps5/dxvk-ps5), `tools/ps5/build.sh runtime`, `tools/ps5/upload_title.sh` (close the title first or the upload of a running eboot can fail; oh2launch closes our own stale run), `tools/ps5/run_title.sh --game-dir "$PS5_DUMP_REMOTE" --env FL_LOG_FILE=/data/homebrew/PPSA99642/logs/runtime.txt --env FL_LOG=loader,d3d --env DXVK_LOG_PATH=/data/homebrew/PPSA99642/logs --env XDG_DATA_HOME=/data/homebrew/PPSA99642/data` (arguments are saved, so a home-screen launch reuses them), then read `logs/runtime.txt` and `logs/_d3d11.log` with `python3 -I tools/ps5/ps5ftp.py cat ...`.
- **Things that bit this session:** titles must not exit (SIGSYS) and must not return from main; ShadowMount+ drops a title whose eboot.bin briefly disappears during a redeploy (ps5ftp renames atomically now); RADV whole-archives Mesa's addrlib and zlib; Meson thin archives and dot-named objects; never put game file sizes in docs (Rule 0: the classifier blocked one).
- Monty is fine with test launches (each failed run shows an error dialog on the TV); crash details come from our log.

## 2026-10-31: Claude: M2 reached on the console (title screen), waiting for the TEST CARD

- Ran the diagnostic build: the "no direct memory" failure was not exhaustion. `sceKernelMapDirectMemory` refuses (EINVAL) addresses in the game's 0x9/0xa/0xb_0000_0000 reservations although the kernel let us reserve them. PS5 `map_anon` with an address now test-maps both ends and refuses such ranges, so they are placed elsewhere like 0x800000000 (FINDINGS 7.8).
- Next run hung with no log: a PS5 write-watch bug (double erase in the merge of `write_watch_register`, then a self-deadlock in the fault hook). Found with `FL_DUMP_THREADS_AT` plus the new `tools/ps5/symbolize.py`; fixed, os_test check added (Linux 23/23; not yet rerun on the console).
- Result: DXVK swap chain on the PS5 display, presents at 30 fps; Monty's TV shows the title screen ("press A") but not the 3D background behind it. Linux check: build, os_test, gcn_layout_test, concrt_token_test pass, FL_EXIT_AFTER_FRAME=600 reached.
- **Next:** Monty answers the M2 TEST CARD; on a pass merge ps5-test into ps5 and tag ps5-M2. Then the missing background: compare a Linux and a PS5 run of the title screen with FL_LOG=d3d,shaders,upload (draw counts per frame: FL_LOG=fps gives dxvk per-frame draws/passes; on Linux at the title screen vs. the PS5), look for PS5-only errors in _d3d11.log, then M3 (launcher) and M4 (ScePad).
