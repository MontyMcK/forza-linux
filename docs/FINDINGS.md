# Findings: confirmed facts about the game binary

Hand-written facts only. No pasted disassembly or game bytes (see CLAUDE.md, Rule 0).
Each entry should say **how it was verified**. Mark guesses with **(unverified)**.

---

## 1. The dump (2026-10-23)

- The archive `forza 2.rar` (RAR5, 39 GiB) holds one top-level folder, `Anthem_1.0.0.64_x64__8wekyb3d8bbwe/`.
  - "Anthem" is FH2's internal codename (source paths say `d:\p4\anthem_rc\...`). `8wekyb3d8bbwe` is Microsoft's publisher ID. This is not EA's Anthem.
- `Mount/` holds the **decrypted** title package: the game exe, `AppxManifest.xml`, the `media/` assets (~2400 files) and a few DLLs.
- `EmbeddedXvd/` is the Xbox "ERA" title OS: Microsoft's System32 DLLs. Use it for reference only (Rule 1).
- Manifest: `Identity Name="Anthem" Version="1.0.0.64"`, `Application Executable="Forza_Durango_Release_Final.exe" EntryPoint="Forza.App"`, `mx:OSName xdk-nov-2012`, `ApplicationEnvironment=title`.
  - Verified: read `Mount/AppxManifest.xml`.

## 2. The executable: `Mount/forza_durango_release_final.exe`

- PE32+, AMD64, GUI subsystem, 58,204,160 bytes on disk.
- ImageBase `0x140000000`, SizeOfImage `0x3A18000` (~58 MB), entry point RVA `0xF18058`.
- Has a `.reloc` section, so it can be relocated, but we should still try to map it at the preferred base.
- Sections: `.text` (0x1DB300F), `BINK`, `RADCODE`, `.rdata`, `.data` (0xAF2568), `.pdata` (0x1F2E28 → full x64 unwind info), `minATL`, `.tls`, `_RDATA`, `BINKDATA`, `BINKCONS`, `RADDATA`, `.rsrc`, `.reloc`.
- **CPU**: plain x86-64 with heavy AVX (VEX-encoded `vmovaps`/`vmulss`…). The Xbox One CPU is AMD Jaguar, so expect at most AVX, F16C, BMI1 and SSE4.2. All of these run natively on Zen, so **no CPU translation is needed**.
  - Verified: `llvm-objdump -d` instruction histogram.
- **TEB/GS usage**: 613 `gs:` references.
  - `gs:0x58` (591×): TEB.ThreadLocalStoragePointer (static `__declspec(thread)` TLS).
  - `gs:0x20` (12×): TEB.NtTib.FiberData. **The game uses fibers.**
  - `gs:0x30` (1×): TEB.Self. `gs:0x10` (1×): StackLimit.
  - So we must point the GS base at a Windows-layout TEB on every thread.
- **RTTI and C++ symbol names are present** (`.?AV...@@` type descriptors). Use them to name classes and vtables.
- **Source paths** are present in assert strings, e.g. `d:\p4\anthem_rc\Engine\App\Src\RenderThread11.cpp`.
- The game is written in C++/CX (WinRT): it imports `vccorlib110.DLL` and starts through `Windows.ApplicationModel.Core.CoreApplication`.

### 2.1 Static imports (648 symbols total)

| DLL | # | Notes |
|---|---|---|
| MSVCR110.dll | 198 | VS2012 C runtime. We must provide our own (Windows ABI!) |
| MSVCP110.dll | 185 | VS2012 C++ standard library (locale, threads `_Mtx_*/_Cnd_*/_Thrd_*`, iostream internals) |
| kernelx.dll | 112 | Xbox kernel32 subset + `XMemAlloc/XMemFree/XMemSetAllocationHooks/XMemAllocDefault/XMemFreeDefault`, `TitleMemoryStatus`, fibers, IOCP, `SetThreadIdealProcessorEx` |
| vccorlib110.DLL | 57 | C++/CX runtime: `Platform::Object/String/Exception`, `GetActivationFactoryByPCWSTR`, event sources, WinRT exception raisers |
| WS2_32.dll | 45 | Sockets (networking can be stubbed offline) |
| combase.dll | 20 | COM + HSTRING (`WindowsCreateString`...), `CoCreateInstance`, `CoCreateFreeThreadedMarshaler` |
| AcpHal.dll | 9 | Xbox audio hardware (SHAPE/XMA): `AcpHalCreate`, `AcpHalAllocateShapeContexts`, `ApuAlloc/ApuFree/ApuCreateHeap`, `ApuMapVirtualAddress`, `ApuMapApuAddress`, `ApuIsVirtualAddressValid` |
| xg_x.dll | 7 | GPU texture tiling-layout helpers: `XGComputeTexture2DLayout`, `XGComputeOptimalTileMode`, `XGCreateTexture{1,2,3}DComputer`... |
| MFPlat.DLL / MFReadWrite.dll | 4+1 | Media Foundation: `MFCreateSourceReaderFromURL` (some video/audio path) |
| EtwPlus.dll | 4 | Telemetry (`EtxRegister`...). Stub as no-ops |
| d3d11_x.dll | 3 | `D3D11XCreateDeviceX`, `D3DMapEsramMemory`, `DXGIXPresentArray`. Everything else goes through COM vtables |
| MMDevAPI.DLL | 2 | `ActivateAudioInterfaceAsync` (WASAPI IAudioClient), `SetWasapiThreadAffinityMask` |
| bcrypt.dll | 1 | `BCryptGenRandom` |

- Dynamically loaded (`LoadLibraryW`/`LoadLibraryExW` + `GetProcAddress`): `xaudio2_9.dll` (string present), plus `Mount/Microsoft.Xbox.Services.dll`, `Microsoft.Xbox.GameChat.dll` and `Microsoft.Xbox.Input.Wheel.dll` **(unverified)**.
- Full list: `llvm-readobj --coff-imports <exe>`. Keep the output in `research/private/`.

### 2.2 WinRT classes referenced (UTF-16 strings in the binary)

Each of these needs an activation factory, or must fail cleanly:
`Windows.ApplicationModel.Core.CoreApplication`, `Windows.ApplicationModel.Package`, `Windows.Data.Json.JsonObject/JsonValue`, `Windows.Foundation.Uri`, `Windows.Kinect.KinectSensor`, `Windows.Media.Devices.MediaDevice`, `Windows.Networking.Connectivity.NetworkInformation`, `Windows.Storage.Streams.Buffer`, `Windows.System.Launcher`,
`Windows.Xbox.Achievements.AchievementNotifier`, `Windows.Xbox.ApplicationModel.Help`, `Windows.Xbox.ApplicationModel.Store.Product`, `Windows.Xbox.Input.Controller`, `Windows.Xbox.Input.BodyController`, `Windows.Xbox.Management.Deployment.{DownloadableContentPackageManager,PackageTransferManager,PackageTransferWatcher}`, `Windows.Xbox.Media.Capture.ApplicationClipCapture`, `Windows.Xbox.Media.GameTransportControls`, `Windows.Xbox.Multiplayer.{Party,MultiplayerSessionReference}`, `Windows.Xbox.Networking.{QualityOfService,SecureDeviceAddress,SecureDeviceAssociationTemplate}`, `Windows.Xbox.Services.XboxLiveConfiguration`, `Windows.Xbox.Speech.Recognition.SpeechRecognizerFactory`, `Windows.Xbox.Storage.ConnectedStorageSpace`, `Windows.Xbox.System.User`, `Windows.Xbox.UI.SystemUI`,
`Microsoft.Xbox.Services.*` (XboxLiveContext, Multiplayer, Presence, Social), `Microsoft.Xbox.GameChat.ChatManager`.

### 2.3 Loader-relevant facts (verified 2026-10-23)

- TLS directory: template 0x150 bytes at rva 0x392e000, `_tls_index` at rva 0x354ebd8, **no TLS callbacks**.
- Load config: security cookie at rva 0x2e68520. 170,286 RUNTIME_FUNCTION entries in `.pdata`.
- The exe has an **export table** of 708 FMOD API functions (`FMOD_System_*`, `?addDSP@ChannelControl@FMOD@@...`), which is handy for naming the audio code.
- The entry point (rva 0xf18058) is the standard MSVC `mainCRTStartup`. It calls `GetSystemTimeAsFileTime` (security cookie), then `__getmainargs`, `_initterm_e`, `_initterm`, then the C++/CX `main`.
- `__chkstk` (rva ~0xf17e7c) reads `gs:0x10` (StackLimit), so StackLimit must be the real lowest stack address.
- Early heap setup: the game reserves 5+ separate 4 GB regions with `VirtualAlloc(type=0x30002000, PAGE_READWRITE)`, i.e. MEM_RESERVE | MEM_LARGE_PAGES | **0x10000000** (an Xbox-only flag, probably graphics/title memory). It then commits inside them with type **0x40000000** (another Xbox flag) plus MEM_COMMIT. Next it installs `XMemSetAllocationHooks(0x141260780, 0x1412608c0)`.
- `TlsAlloc` is imported but `TlsGetValue`/`TlsSetValue` are not. Our TLS slots live in TEB.TlsSlots (0x1480) in case the game reads them inline.
- Startup order observed: `Platform::Details::InitializeData(2)`; static initializers construct `Concurrency::critical_section`s and `concurrent_queue`s and boxed value types; the first `CreateThread` (entry rva 0x30b800, 512 KB stack, CREATE_SUSPENDED, affinity 0x8, priority 2); then main activates `Windows.ApplicationModel.Core.CoreApplication` with ICoreApplication IID {0AACF7A4-5E1D-49DF-8034-FB6A68BC5ED1}.

## 3. Graphics

- The renderer is Direct3D 11.X (Xbox D3D11 with extensions). The source file names end in `11.cpp` (RenderThread11, SkidMarks11, Presentation11).
- **Shaders are standard DXBC, Shader Model 5.** Files `media/shaderpatch/*.durango.vso/.pso` start with the `DXBC` magic and contain only `ISGN`, `OSGN` and `SHEX` chunks. There is no Xbox GCN microcode chunk, so DXVK's DXBC→SPIR-V compiler should handle them as-is.
  - Verified: hex-dumped two sample `.vso`/`.pso` files.
- `.shaderbin` files are a Forza container (magic `burG`, chunks `ECSL`, `SLBD`, `PMBC`, `PMXT`…). They probably wrap DXBC and material bindings **(unverified)**.
- `xg_x` usage means the game computes **tiled texture layouts itself** and probably uploads pre-tiled (GCN tile mode) texture data, or creates placement resources over its own memory. Untiling may be required **(unverified, high priority to check)**.
- **First D3D call** (render thread, rva 0x10d8839 → import thunk 0x19928ce): `D3D11XCreateDeviceX(params, ID3D11DeviceX** dev, ID3D11DeviceContextX** ctx)`. The params struct is on the stack: `+0x00 u32 0x1000B` (probably a version), `+0x04 u32 0x04000000` (flags), `+0x08` 16 bytes of 0, `+0x18 u64 0x3F` (probably a core/affinity mask), `+0x20 u64 0`, `+0x28 u32 0`. It's called right after the game loop's first `CoreDispatcher::ProcessEvents(3)`.
- `D3DMapEsramMemory`: the game places some resources in the 32 MB ESRAM. Emulate it as ordinary GPU memory.
- `EmbeddedXvd/.../d3d11_x.dll` (17 KB) is only a loader: it loads `umd.dll` (or `umd_d`/`umd_v`/`umd_i.dll`, the debug/validation/instrumented builds, all in the same System32) and calls its exports `UMDCreateDevice`, `UMDPresentArray`, `UMDCreateBlob`, `UMDGetDebugInterface`. **`umd.dll` is the real D3D11.X runtime** and our RE reference for every slot (verified: strings and imports of d3d11_x.dll). It has no RTTI, so use `tools/pe_re.py` (vtables / slots / dis / findguid).

### 3.1 D3D11.X object layouts (verified 2026-10-24 in umd.dll + game call sites)

Addresses below are umd.dll RVAs unless they say "game rva".

**The extra base slot.** Every Xbox D3D/DXGI interface has **one extra method right after `SetPrivateDataInterface`** compared to the public SDK. So Xbox slot = desktop slot + 1 from that point on: ID3D11Device from slot 37, IDXGIObject-based from slot 5, ID3D11DeviceChild-based (context, resources…) from slot 7. Evidence: in each vtable, two adjacent slots have the SetPrivateDataInterface shape (GUID, pointer) and every later slot matches the desktop method one position earlier (device: slot 38 returns 0xB100 = GetFeatureLevel, 39 returns a field = GetCreationFlags, 24 and 25 share code = CreateQuery/CreatePredicate, 28 returns E_NOTIMPL = OpenSharedResource, 33 switches on the feature enum = CheckFeatureSupport). What the extra method does is unknown (it uses the same private-data store).

**ID3D11DeviceX**: 84 slots, vtable at 0x16f2e0 (in `.data`), installed by `UMDCreateDevice` (0x2c770). Slots 0–53 = desktop ID3D11Device2 order with the +1 shift from 37 (so 0–54 used). Slots 55–83 are Xbox-only. Slot 55 uses the same function as GetImmediateContext (→ "GetImmediateContextX"). Several Xbox slots return E_NOTIMPL. `src/hle/d3d11x/desktop_slots.h` (generated) has the desktop numbers.
- **Slot 59** (umd 0x2b2b0 → 0x27ed0) creates a 0xA8-byte device child with a **30-slot vtable at 0x16f580**: `(const desc*, out**)`. Desc: `+0 u32` flags whose low 4 bits pick an engine (the game passes 1 and 2), `+4 u32` ring size (game 0x10000, 0 means 0x80000), `+8 u32` 0. The game creates two at game rva 0x10d8afb / 0x10d8b3d and keeps them at renderer+0x120 / +0x128. The object's slot 8 returns 2, slots 19–25 take 64-bit GPU virtual addresses and reject anything ≥ 1<<40. **Probably the DMA/copy-engine contexts (name unverified).** This is the first unimplemented slot the game hits.

**IDXGIDevice (Xbox)**: 18 slots at 0x16f6c0 = desktop IDXGIDevice2 (17) + the extra slot 5. Slot 8 = GetAdapter (the game calls it at game rva 0x10d8980). Slots 9–17 return DXGI_ERROR_UNSUPPORTED (0x887A0004). IDXGIAdapter slot 8 = EnumOutputs (game passes `(0, &out)` at game rva 0x10d89a2, keeps the output at renderer+0x28).

**Xbox-only IIDs** (umd QueryInterface returns the object itself for each):
- `{88671610-712E-4F1E-84AB-01B5948BD373}` on the device (umd 0x28627). We call it ID3D11DeviceX. The game QIs for it and keeps it at renderer+0x18, then calls slot 59 on it.
- `{9458FE06-C78D-47F7-96A0-EC7B727BE1E9}` on the context (umd context QI 0x8c33, checked first): probably ID3D11DeviceContextX.
- `{48800095-7134-4BE7-9186-B86BECB26477}` on the context (umd generic QI 0x336f). Identity unknown. The game keeps it at renderer+0x30 and does not null-check it.

**The immediate context is a memory block the game pokes directly.**
- umd's context constructor (0x266e0 / 0x267f0) **copies the 0x828-byte (261-slot) method table from 0xd6950 into the context at +0x3D0** and sets the context's vptr to context+0x3D0. (The vtable scanner shows the source as 253 slots: the last 8 entries aren't code pointers.) It also stores 1 at +0x8. AddRef/Release forward to the device's refcount through the device pointer at +0x10.
- **The game inlines context methods.** 54 inlined sites read the byte at **context+0x390**: if **bit 1** is set they call through the embedded table (`call [ctx + 0x3D0 + 8*slot]`), otherwise they write state into the block themselves. Example, game rva 0xc56060: IASetInputLayout inline stores the layout at ctx+0x178 and ORs 0x89 into ctx+0x168 (the dirty flags), or calls ctx+0x460 = slot 18. umd's slot 18 does the same writes.
- **Some inlined writes are not guarded by the bit**: e.g. game rva 0xc5b74e, 0x10c3fa6, 0x10de370 store a packed 64-bit value (0x3FFC0009E00 / 0x9FFC0009E00) at ctx+0x170 and OR 0x46 into ctx+0x168. Our draw code will have to read those fields. Meaning unknown.
- **Context slot map (verified across all 261 slots in umd, 2026-10-24):** 0–6 IUnknown/DeviceChild like desktop, **7 = the extra base slot**, **8–24 = desktop 7–23 (+1)**, **desktop 24 IASetPrimitiveTopology has NO slot** (the game always inlines it, see below), **25–143 = desktop 25–143 exactly** (checked e.g. 83 IAGetPrimitiveTopology writes a dword, 72/77/81/100/104/109 have the constant-buffer-getter shape, 112/113 GetType/GetContextFlags return 0, 114 FinishCommandList E_NOTIMPL on the immediate context, 115/116 reuse the CopySubresourceRegion/UpdateSubresource code). **144–260 are Xbox-only.** `ctx_slot()` in d3d11x.h encodes this. Exception: **slot 20 (index buffer) has an Xbox signature** `(this, hw index type (0 = 16-bit, 1 = 32-bit), buffer, offset)`.
- **The inlined IASetPrimitiveTopology** stores `table[topology]` (a u64) at ctx+0x170 and ORs 0x46 into ctx+0x168, where `table` is a 65-entry u64 array in the game image at **game rva 0x1df87e0** (bits 41+ = GCN primitive type). We read that table at run time to map the value back (context.cpp `apply_inline_state`, called before every draw).
- **Byte ctx+0x390 has two mode bits the inlined code checks:** bit 1 (55 sites): inlined state setters call the table instead of writing fields. **Bit 0 (37 sites): the game writes raw command-buffer packets itself** at ctx+0x50 (write pointer, limit +0x58, slot 208 = "make room"); with bit 0 set it instead calls **slot 209 with compact commands**: 20 bytes each, `u32 op` (bit 31 bind, bits 28–30 kind 0 SRV / 1 CB / 2 sampler, bits 16–19 stage 0 VS 1 HS 2 DS 3 GS 4 PS 5 CS, bits 8–15 slot, byte 0 = 1), `u64 object`, `u64 extra` (0 everywhere seen). Evidence: register bases match umd's GS setters (CB 0x569 = slot 23, sampler 0x67d = slot 32, SRV 0x57d0 = slot 31). We set both bits.
- Context Xbox-only slots seen so far: **196** `(BOOL)` sets/clears bit 4 of umd's internal byte +0xc11 (no other effect; we store it). **160–193**: per-stage binds by CPU pointer into a buffer (e.g. 180 = PS constant buffer: GPU address = pointer − buffer CPU base + buffer GPU base) — not implemented yet.
- **Deferred contexts** use the same block layout (umd copies 0x828 bytes from table 0x117b60 to +0x3d0). Device slot 27 = CreateDeferredContext with Xbox flags 0x60000/0xa0000 (meaning unknown; DXVK needs 0).
- **Resources:** Xbox resource descriptors = desktop + 2 trailing u32 (umd copies them to the same internal fields for every type; probably ESRAM offset/size). MiscFlags bits 0x80000/0x100000/0x200000 are Xbox-only. Resource vtable (13 slots, umd 0x16f750): 8 GetType, 9/10 eviction priority, **11 Xbox extra (`*out = this`)**, 12 GetDesc. Views: 8 GetResource, 9 GetDesc (SRVs may have 2 more). State objects: 8 GetDesc (9 GetDesc1).
- **Device Xbox-only slots:** **62 CreatePlacementBuffer(desc, address, out)**, **63/64/65 CreatePlacementTexture1D/2D/3D(desc, tile mode index, pitch, address, out)** (umd packs `tile<<24 | pitch` into the first Xbox desc field, ORs MiscFlags 0x40000000, creates a normal resource, binds the address), 71 = 2D placement variant over other memory (not implemented), **69 = process deferred frees** (a worker thread calls it in a loop; no-op for us).
- `d3d11_x!D3DMapEsramMemory(flags, address, page count, page list)`: maps 64 KB ESRAM pages into a reserved range (game call rva 0x10c5a9d). `d3d11_x!DXGIXPresentArray(sync, flags, ?, count, swap chains, params)` (rva 0x10f484d); the game creates its swap chain with IDXGIFactory2::CreateSwapChainForCoreWindow (Xbox factory slot 17) on the factory from adapter GetParent (slot 7).
- The real context struct is large (umd uses fields up to at least +0x2b0c0). Our ContextX allocates 0x40000 bytes.

**The game's device setup** (game rva 0x10d8770…0x10d8b40): `D3D11XCreateDeviceX` → QI device for ID3D11Device (renderer+0x8) → QI context for ID3D11DeviceContext → store feature level 0xB100 at renderer+0x38 → QI device for IDXGIDevice1 → GetAdapter → EnumOutputs(0) → QI context for 9458FE06 and 48800095 → QI device for 88671610 → device slot 59 twice.

### 3.2 Texture layouts (xg_x) and tiling (verified 2026-10-24)

- **Assets are stored pre-tiled.** `.swatchbin` textures (`burG` container: `BCXT`/`HCXT` chunks; header at +0x40: u16 width, height, depth, …, u8 mip count, format code) decode to scrambled 128-pixel column blocks as linear data.
- **The console GPU tiling:** GB_TILE_MODE0–31 and GB_MACROTILE_MODE0–15 tables at xg_x.dll rva 0x16648 / 0x166c8 (standard GCN 1.1 4-pipe P4_16x16 values; listed in src/hle/xg/gcn.cpp), pipe interleave 256 B and DRAM row 2 KB (xg code at rva 0x6559/0x6560). With AMD addrlib (Mesa, MIT) configured this way (`GB_ADDR_CONFIG = 0x10001002`, family CI), our layouts match the console's XG **exactly** for real assets (tests/gcn_layout_test.cpp: 4/4 sizes, incl. full mip chains — every level is padded to a power of two), and the untiler turns a real BC4 texture into the correct image (tools/swatch_view.cpp).
- **XGComputeOptimalTileMode** (xg rva 0x12f0 → 0x4140), reimplemented in xg.cpp: buffers 0x1F, depth binds → -1, 1D → 8 (linear aligned), 3D → 13, 2D with MiscFlags bit 27 → 16/17 (PRT), else 14 if width/height reach a minimum from a 7-entry table (rva 0x14b58) indexed by log2(min(clamp(2·bpe·64, 256, 2048), bpe·64·samples)) − 6, otherwise 13. **XGComputeOptimalDepthStencilTileModes**: depth = stencil = {1 sample: 0, 2/4: 1, 8: 2}.
- **XG_TEXTURE2D_DESC** = Xbox D3D11 desc (0x34) + TileMode + Pitch (1D/3D the same pattern). **XG_RESOURCE_LAYOUT** is 0xd40 bytes: u64 size, u64 alignment, u32 mips, u32 planes, 3 × 0x460-byte planes {u32 usage, u64 size, u64 base offset, u64 alignment, u32 bytes per element @+0x20, 15 × 0x48-byte mips {u64 size, u64 offset @+8, u64 slice size @+0x10, pitch px, pitch bytes @+0x1c, …, width/height/depth in elements @+0x30/34/38, samples, tile mode}}. Evidence: xg's GetResourceLayout copies 0xd40 bytes; xg's methods read +0x08, +0x30–0x38, plane+0x20; the game's untile loop (rva 0x10c3860) uses the same fields. The XG "texture address computer" is NOT COM: 0 AddRef, 1 Release, 2 GetResourceLayout, 3 GetMipLevelOffsetBytes(plane, mip), 4 GetTexelElementOffsetBytes(plane, mip, x, y, z, sample) (vtable rva 0x14670).

### 3.3 Unified memory (our design)

Placement resources live in game memory; DXVK resources don't. `src/hle/d3d11x/gpumem.*` write-protects uploaded pages; the first write faults, the fault hook (loader/crash.h) marks the page dirty and unprotects. Binding a placement resource uploads it if dirty (buffers: UpdateSubresource; textures: untile each mip/slice). Syscalls into protected pages fail with EFAULT instead of faulting, so `ReadFile` calls `gpumem::prepare_write` first. Render targets/depth/UAVs are GPU-owned and never uploaded. Map on a placement buffer returns the game memory itself.

## 4. Audio

- **FMOD Studio** is statically linked (FMOD C++ symbols in the exe). Banks are `media/audio/**/*.bank`, RIFF `FEV ` format (740 files).
- FMOD outputs through WASAPI (`ActivateAudioInterfaceAsync` → IAudioClient/IAudioRenderClient) **(unverified, but the imports suggest it)**, and decodes XMA through the hardware SHAPE unit (`AcpHal`).
- `xaudio2_9.dll` is referenced by name. It may be used by Bink or as an FMOD output mode.

## 4b. WinRT interfaces (verified from game call sites)

**The Xbox WinRT interfaces differ from desktop Windows.** Never assume the desktop slot order. Verify each one by identifying the delegate types or argument shapes at the call sites (RTTI names the delegate classes).

- `ICoreWindow` (Xbox): `S+1` get_Dispatcher (rva 0xf74a5e; **desktop S+1 is get_Bounds, so do not guess**); `S+5` Activate() (no args, rva 0xf6c858); `S+9` put_PointerCursor (CoreCursor arg, rva 0xf6c692). Events: `S+17/18` add/remove KeyDown, `S+19/20` KeyUp (TypedEventHandler<CoreWindow,KeyEventArgs>, rva 0xf6c746/0xf6c7fd), `S+21..S+28` four unidentified event pairs, `S+29/30` VisibilityChanged (rva 0xf6c5db). (S = 6, the IInspectable slots.)
- `ICoreApplication`: desktop order works (get_Id, add/remove Suspending, add/remove Resuming, get_Properties, GetCurrentView, Run, ...). The Xbox adds statics interface `{9FF3799E-6E83-468C-A5B0-501CE6BB0FA1}`: `S+0` get_ResourceAvailability(int*), `S+1/S+2` add/remove ResourceAvailabilityChanged.
- `PackageTransferManager` statics `{D6CF4CAE-...}`: `S+0` get_Current. Instance `S+1`: `(uint32 chunk, bool* installed)` (rva 0x11d03c4).
- WASAPI: `MMDeviceEnumerator` is QI'd for Xbox-only `{8B557ADC-555E-47A0-B223-43477E481DAD}`: slot 8 `(uint32*, uint32*)` hardware channel counts, slot 9 registers a callback (game class `HardwareChannelCount`, rva 0xff5d08).

## 4c. Xbox WinRT interfaces: the proxy data (2026-10-24)

The Xbox OS classes (User, Controller, ConnectedStorage, …) run outside the title, so their code isn't in the dump, but **EmbeddedXvd's `gameosproxy.dll` (and appmodel/ConnectedStorage/Chat/... DLLs) contain the MIDL proxy tables**: interface name, IID, method count, and each method's parameters (direction, base types, interface types with IIDs, HSTRINGs) — 800+ interfaces, including parameterized ones like `IVectorView<User>` with their real IIDs. `tools/midl_proxies.py <dll> [filter]` dumps them; proc number = vtable slot. Method names are not included; event delegate types name the events. This is how `Windows.Xbox.System.User` (src/hle/winrt/xbox_system.cpp) was built.

## 4d. Package DLLs

The package manifest declares in-process WinRT servers in the package itself: `Microsoft.Xbox.Services.dll` (XboxLiveContext and ~15 other classes), `Microsoft.Xbox.GameChat.dll`, `Microsoft.Xbox.Input.Wheel.dll`. They import only msvcr110/msvcp110/vccorlib110/kernelx/combase. We load them with our PE loader (loader/modules.*) and activate classes through their own `DllGetActivationFactory` (activation.cpp reads the manifest). All three have base 0x180000000 and no TLS. 83 imports are missing for them, 45 of which the game exe needs too (C++/CX exceptions, PPL tasks, locale).

## 5a. C++ runtime layouts (verified from game code)

- `Concurrency::concurrent_queue<T>` base `_Concurrent_queue_base_v4`: `{vfptr, rep*, items_per_page @0x10, item_size @0x18}`. The derived vtable is `0 _Move_item(page&, idx, src)`, `1 _Copy_item`, `2 _Assign_and_destroy_item(dst, page&, idx)`, `3 scalar deleting dtor`, `4 _Allocate_page()`, `5 _Deallocate_page(page*)`. A page is a 16-byte header followed by items (`item = page + 16 + idx*item_size`). Verified by disassembling the vtable at rva 0x2c66210.
- C++/CX boxed values: the ctor stores `GetIBoxVtable(this)` / `GetIBoxArrayVtable(this)` at `this+8`, replacing the compiler's IPropertyValue vtable (seen at rva 0x1986b98).

## 5. Video and other assets

- Bink 2 (`.bk2`, 15 files) is statically linked (sections `BINK`, `RADCODE`). It runs as normal game code.
- `.zip` (584): ordinary zip archives. `.minizip` (24): magic `PGZP` (Forza container). `.bank`: FMOD. Also `.nt`, `.owt`, `.tz`, `.avpins`, `.lightdb`, `.pvs`… We don't need to understand these, because the game parses them itself.
- The game opens assets as **`g:\media\...`**, falling back to `.\media\...` (verified via the file log). `G:` is the package root, `src/hle/kernelx/vfs.cpp` maps it to `--game-dir`, and lookups are case-insensitive (e.g. `audioglobals.xml` resolves to `AudioGlobals.xml`).
- The game checks `CRITICAL_SECTION.DebugInfo != 0` as an "initialized" flag (spin loop at rva 0x13d0822), so InitializeCriticalSection must set DebugInfo to non-zero (we use -1, as Windows does without debug info).
