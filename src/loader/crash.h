#pragma once

#include <cstdint>

#include <string>

namespace fl::loader {

// Install SIGSEGV/SIGBUS/SIGILL/SIGFPE handlers that print a useful report
// (fault address, registers, game RVAs) before dying.
void install_crash_handler();
// FL_BREAK one-shot breakpoints in game code (after the image is mapped).
void arm_breakpoints();

// Called first for every SIGSEGV. Return true if the fault was handled (the
// faulting instruction is retried). Must be async-signal-safe.
void set_fault_hook(bool (*hook)(uint64_t addr, bool write));

// Reserve an inaccessible page standing in for an unimplemented data import.
// Touching it is reported as "UNIMPLEMENTED data import <dll>!<name>".
void* make_data_trap(const std::string& dll, const std::string& name);
// Every Present calls this. A watchdog logs a thread dump (channel "threads")
// when no frame was presented for 15 s, once per freeze; FL_WATCHDOG=0 turns
// it off. It only logs: a freeze report then comes with where every thread is.
void note_present();

}  // namespace fl::loader
