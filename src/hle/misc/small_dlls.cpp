// Small system DLLs: EtwPlus (telemetry), bcrypt (random), Media Foundation startup.

#include <sys/random.h>

#include "common/log.h"
#include "common/winabi.h"
#include "hle/registry.h"
#include "platform/os/random.h"

namespace fl::misc {
namespace {

// ---------------------------------------------------------------- EtwPlus: telemetry, all no-ops
// Signatures are not RE'd; returning 0 (ERROR_SUCCESS) and touching nothing is safe.

WINAPI ULONG etx_Register(void*, void*, void*, void*) { return 0; }
WINAPI ULONG etx_Unregister(void*) { return 0; }
WINAPI ULONG etx_EventWrite(void*, void*, void*, void*) { return 0; }
WINAPI ULONG etx_FillCommonFields(void*, void*, void*) { return 0; }

// ---------------------------------------------------------------- bcrypt

WINAPI NTSTATUS b_BCryptGenRandom(void* alg, uint8_t* buf, ULONG len, ULONG flags) {
    os::random_bytes(buf, len);
    return 0;
}

// ---------------------------------------------------------------- Media Foundation

WINAPI HRESULT mf_MFStartup(ULONG version, DWORD flags) {
    FL_LOG(media, "MFStartup(0x%x)", version);
    return S_OK;
}
WINAPI HRESULT mf_MFShutdown() { return S_OK; }

}  // namespace

HLE_EXPORT(etwplus, EtxRegister, etx_Register);
HLE_EXPORT(etwplus, EtxUnregister, etx_Unregister);
HLE_EXPORT(etwplus, EtxEventWrite, etx_EventWrite);
HLE_EXPORT(etwplus, EtxFillCommonFields_v7, etx_FillCommonFields);
HLE_EXPORT(bcrypt, BCryptGenRandom, b_BCryptGenRandom);
HLE_EXPORT(mfplat, MFStartup, mf_MFStartup);
HLE_EXPORT(mfplat, MFShutdown, mf_MFShutdown);

}  // namespace fl::misc
