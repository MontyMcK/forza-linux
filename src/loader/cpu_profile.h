// FL_CPU_PROFILE=<start s>:<length s>: a sampling CPU profile of the whole
// process (SIGPROF every millisecond of process CPU time, so samples land on
// whichever thread is busy). At the end it logs (channel "cpuprof") the
// samples per thread and the hottest code: game functions by rva (.pdata),
// native code by module + offset (symbolize with llvm-symbolizer).
#pragma once

namespace fl::loader {

void start_cpu_profiler();  // call once after the image is mapped

}  // namespace fl::loader
