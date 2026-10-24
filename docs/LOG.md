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
