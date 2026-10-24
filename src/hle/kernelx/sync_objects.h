// Concrete waitable kernel objects. All fields are guarded by wait_lock().
#pragma once

#include "hle/kernelx/kobject.h"

namespace fl::kx {

struct Event : Waitable {
    Event() : Waitable(ObjType::Event) {}
    bool manual = false;
    bool state = false;
    bool signaled(DWORD) const override { return state; }
    void consume(DWORD) override {
        if (!manual) state = false;
    }
};

struct Mutex : Waitable {
    Mutex() : Waitable(ObjType::Mutex) {}
    DWORD owner = 0;
    uint32_t count = 0;
    bool signaled(DWORD tid) const override { return count == 0 || owner == tid; }
    void consume(DWORD tid) override {
        owner = tid;
        count++;
    }
};

struct Semaphore : Waitable {
    Semaphore() : Waitable(ObjType::Semaphore) {}
    LONG count = 0;
    LONG max = 0;
    bool signaled(DWORD) const override { return count > 0; }
    void consume(DWORD) override { count--; }
};

// SetEvent for use by other HLE code (file I/O completion etc.).
bool signal_event(HANDLE h);
bool reset_event(HANDLE h);
HANDLE create_event(bool manual, bool initial);
DWORD wait_multiple(DWORD n, const HANDLE* handles, bool all, DWORD ms);

}  // namespace fl::kx
