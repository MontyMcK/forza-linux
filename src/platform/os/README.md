# src/platform/os: what the runtime needs from the host OS

One header per area here; one implementation per OS in `src/platform/os/<os>/`.
CMake builds the backend named by `FL_OS` (default `linux`) as the `fl_os` library
(`ps5/` passes tests/os_test.cpp on the console: `tools/ps5/build.sh os-test-title`); the PS5 backend
(`ps5/`) comes with docs/PS5_PORT.md Step 6. Code outside this folder must not
call the OS directly for anything listed below.

| Area | Header | Linux | PS5 (planned, FINDINGS §7) | Moved |
| --- | --- | --- | --- | --- |
| Futex wait/wake | futex.h | `SYS_futex` (private) | `_umtx_op` WAIT_UINT_PRIVATE / WAKE_PRIVATE | yes |
| File existence | files.h | `stat` | `stat` (never `access`: EPERM in a title) | yes |
| Memory: reserve, commit, protect, exec, page size | memory.h | mmap / mprotect | ReserveVirtualRange, direct memory, sceKernelMprotect | yes |
| Thread environment: GS (TEB), game-thread CPU state, thread ids, signal a thread | thread.h | `arch_prctl(ARCH_SET_GS)`, `gettid`, `tgkill` | `sysarch(AMD64_SET_GSBASE)`, MXCSR 0x1f80, `thr_self`, `thr_kill` | yes |
| Checked copies (never fault) | memory.h | `process_vm_readv/writev` on ourselves | a fault-guarded copy (no process_vm_*) | yes |
| Signal context access | signal_context.h | `ucontext_t` gregs / fpregs | mcontext at +64 (rip +224), FXSAVE at +320 | yes |
| Window / present | window.h | SDL3 window (DXVK's swap chain is made for it) | VK_KHR_display on VideoOut | yes |
| Input | input.h | SDL3 gamepad / keyboard | ScePad | yes |
| Controller output (rumble, triggers) | input.h (`set_vibration`) | SDL3 | ScePad | yes |
| Audio out | audio.h | SDL3 audio stream | SceAudioOut | yes |
| GPU write tracking (GetWriteWatch-like) and page probes | write_watch.h | userfaultfd async WP + PAGEMAP_SCAN, `msync`, `process_vm_readv` | mprotect + SIGSEGV (~41 us/fault, FINDINGS 7.3) | yes |
| Diagnostics: thread list, names, CPU times, the mapping of an address | debug.h | `/proc/self/task`, `/proc/self/maps` | (to find; may return empty) | yes |

Plain POSIX (files, pthreads, clocks, `sigaction`, `sigaltstack`, real-time
signals) stays in the callers: the PS5's FreeBSD libc has it. Watch for the
PS5's limits there: ~249 files open by path, 64 KiB default thread stacks
(FINDINGS 7).
