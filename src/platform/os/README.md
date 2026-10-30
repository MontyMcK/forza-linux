# src/platform/os: what the runtime needs from the host OS

One header per area here; one implementation per OS in `src/platform/os/<os>/`.
CMake builds the backend named by `FL_OS` (default `linux`); the PS5 backend
(`ps5/`) comes with docs/PS5_PORT.md Step 6. Code outside this folder must not
call the OS directly for anything listed below.

| Area | Header | Linux | PS5 (planned, FINDINGS §7) | Moved |
| --- | --- | --- | --- | --- |
| Futex wait/wake | futex.h | `SYS_futex` (private) | `_umtx_op` WAIT_UINT_PRIVATE / WAKE_PRIVATE | yes |
| File existence | files.h | `stat` | `stat` (never `access`: EPERM in a title) | yes |
| Memory: reserve, commit, protect, exec, page size | memory.h | mmap / mprotect | ReserveVirtualRange, direct memory, sceKernelMprotect | yes |
| Thread environment (GS = TEB) | | `arch_prctl(ARCH_SET_GS)` | `sysarch(AMD64_SET_GSBASE)` | no |
| Signal context access | | `ucontext_t` / `REG_*` | mcontext at +64, FXSAVE at +320 | no |
| Window / present | | SDL3 | VK_KHR_display on VideoOut | no |
| Input | | SDL3 gamepad | ScePad | no |
| Controller output (rumble, triggers) | | SDL3 | ScePad | no |
| Audio out | | SDL3 audio stream | SceAudioOut | no |
| GPU write tracking | | userfaultfd + PAGEMAP_SCAN | mprotect + SIGSEGV | no |
