#include "loader/thread_env.h"

#include <cstdlib>
#include <cstring>

#include "common/log.h"
#include "loader/pe_image.h"
#include "loader/pe_structs.h"
#include "platform/os/thread.h"

namespace fl::loader {
namespace {

// Minimal PEB. Offsets: 0x10 ImageBaseAddress, 0x30 ProcessHeap.
uint8_t* g_peb = nullptr;

const ImageTlsDirectory64* g_tls = nullptr;
size_t g_tls_size = 0;

using TlsCallback = void(WINAPI*)(void* dll_handle, DWORD reason, void* reserved);

void run_tls_callbacks(DWORD reason) {
    if (!g_tls || !g_tls->AddressOfCallBacks) return;
    auto* cb = reinterpret_cast<TlsCallback*>(g_tls->AddressOfCallBacks);
    for (; *cb; cb++) {
        FL_LOG(loader, "TLS callback %p reason %u", reinterpret_cast<void*>(*cb), reason);
        (*cb)(main_image().base, reason, nullptr);
    }
}

}  // namespace

// Host-side copy of the TEB pointer (glibc TLS via FS), so non-game threads
// can call current_teb() safely and get null.
thread_local Teb* t_teb = nullptr;

Teb* current_teb() { return t_teb; }

void init_process() {
    PeImage& img = main_image();
    g_peb = static_cast<uint8_t*>(calloc(1, 0x1000));
    *reinterpret_cast<void**>(g_peb + 0x10) = img.base;
    *reinterpret_cast<void**>(g_peb + 0x30) = reinterpret_cast<void*>(0x1);  // fake process heap handle

    if (img.tls_dir_rva) {
        g_tls = img.at<ImageTlsDirectory64>(img.tls_dir_rva);
        g_tls_size = g_tls->EndAddressOfRawData - g_tls->StartAddressOfRawData + g_tls->SizeOfZeroFill;
        // Single module, so its TLS index is 0.
        *reinterpret_cast<uint32_t*>(g_tls->AddressOfIndex) = 0;
        FL_LOG(loader, "static TLS: %zu bytes", g_tls_size);
    }
}

Teb* attach_thread(void* stack_lo, void* stack_hi, bool main_thread) {
    // GS starts at 0 on a fresh thread; reading gs:0x30 would fault, so we
    // can't check current_teb() here.
    auto* teb = static_cast<Teb*>(aligned_alloc(4096, sizeof(Teb)));
    memset(teb, 0, sizeof(Teb));
    teb->field<void*>(teb_off::ExceptionList) = reinterpret_cast<void*>(-1);
    teb->field<void*>(teb_off::StackBase) = stack_hi;
    teb->field<void*>(teb_off::StackLimit) = stack_lo;
    teb->field<void*>(teb_off::DeallocationStack) = stack_lo;
    teb->field<void*>(teb_off::Self) = teb;
    teb->field<uint64_t>(teb_off::ClientIdProcess) = os::process_id();
    teb->field<uint64_t>(teb_off::ClientIdThread) = os::thread_id();
    teb->field<void*>(teb_off::Peb) = g_peb;

    if (g_tls) {
        auto** slots = static_cast<void**>(calloc(1, sizeof(void*) * 8));
        auto* block = static_cast<uint8_t*>(aligned_alloc(64, (g_tls_size + 63) & ~size_t(63)));
        size_t tmpl = g_tls->EndAddressOfRawData - g_tls->StartAddressOfRawData;
        memcpy(block, reinterpret_cast<void*>(g_tls->StartAddressOfRawData), tmpl);
        memset(block + tmpl, 0, g_tls->SizeOfZeroFill);
        slots[0] = block;
        teb->field<void**>(teb_off::ThreadLocalStoragePointer) = slots;
    }

    if (!os::set_gs_base(teb)) fatal("setting the GS base failed");
    os::prepare_game_thread();
    t_teb = teb;
    FL_LOG(loader, "thread attached: teb %p stack %p-%p", static_cast<void*>(teb), stack_lo, stack_hi);
    run_tls_callbacks(main_thread ? 1 /* DLL_PROCESS_ATTACH */ : 2 /* DLL_THREAD_ATTACH */);
    return teb;
}

void detach_thread() {
    Teb* teb = current_teb();
    if (!teb) return;
    run_tls_callbacks(3 /* DLL_THREAD_DETACH */);
    if (auto** slots = teb->field<void**>(teb_off::ThreadLocalStoragePointer)) {
        free(slots[0]);
        free(slots);
    }
    os::set_gs_base(nullptr);
    t_teb = nullptr;
    free(teb);
}

}  // namespace fl::loader
