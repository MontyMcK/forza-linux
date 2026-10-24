#pragma once

#include <cstdint>

#include <string>

namespace fl::loader {

// Install SIGSEGV/SIGBUS/SIGILL/SIGFPE handlers that print a useful report
// (fault address, registers, game RVAs) before dying.
void install_crash_handler();

// Called first for every SIGSEGV. Return true if the fault was handled (the
// faulting instruction is retried). Must be async-signal-safe.
void set_fault_hook(bool (*hook)(uint64_t addr, bool write));

// Reserve an inaccessible page standing in for an unimplemented data import.
// Touching it is reported as "UNIMPLEMENTED data import <dll>!<name>".
void* make_data_trap(const std::string& dll, const std::string& name);

}  // namespace fl::loader
