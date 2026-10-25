// Windows.Xbox.ApplicationModel.Store. Slot signatures from gameosproxy.dll's
// proxy data (FINDINGS §4c). Offline: every privilege check answers
// Restricted, as on a console without Xbox Live (the Azimuth reference does
// the same, FINDINGS §4g); a "granted" multiplayer privilege makes the game
// query Xbox Live sessions after the title screen and die when that fails.
// The store UI methods (S+0..S+4: purchase/details UI) trap until needed.
#include "common/log.h"
#include "hle/winrt/activation.h"
#include "hle/winrt/async.h"
#include "hle/winrt/object.h"

namespace fl::winrt {
namespace {

constexpr int S = kInspectableSlots;
constexpr GUID IID_IProductStatics = {0x76EFB705, 0xBDD4, 0x4F6A, {0x8D, 0x89, 0xA9, 0x51, 0x1D, 0x40, 0xAF, 0xE7}};
// IAsyncOperation<PrivilegeCheckResult>
constexpr GUID IID_IAsyncOp_PrivilegeCheckResult = {0x08CA89DE, 0x0AF8, 0x5333,
                                                    {0x92, 0xB2, 0xFC, 0x5B, 0x0F, 0x58, 0x16, 0x1F}};
// PrivilegeCheckResult: Azimuth's answer (rva 0x16ca20) is 8, its "Restricted".
constexpr uint32_t kPrivilegeRestricted = 8;

struct ProductStatics : Object {
    EventSource purchased;
    // S+5: (user, privilege id, attempt resolution, friendly message, out op). Game call at rva 0x1249e26.
    static WINAPI HRESULT CheckPrivilegeAsync(Iface*, void* user, uint32_t privilege, uint8_t resolve, HSTRING msg,
                                              void** out) {
        FL_LOG(winrt, "Store::CheckPrivilegeAsync(privilege %u) -> restricted (offline)", privilege);
        *out = async_result_u32(IID_IAsyncOp_PrivilegeCheckResult, kPrivilegeRestricted);
        return S_OK;
    }
    // S+6: (user, IVectorView<uint32> privileges, attempt resolution, message, out op).
    static WINAPI HRESULT CheckPrivilegesAsync(Iface*, void* user, void* privileges, uint8_t resolve, HSTRING msg,
                                               void** out) {
        FL_LOG(winrt, "Store::CheckPrivilegesAsync -> restricted (offline)");
        *out = async_result_u32(IID_IAsyncOp_PrivilegeCheckResult, kPrivilegeRestricted);
        return S_OK;
    }
    static WINAPI HRESULT add_ProductPurchased(Iface* i, void* h, EventToken* t) {
        return i->as<ProductStatics>()->purchased.add(h, t);
    }
    static WINAPI HRESULT remove_ProductPurchased(Iface* i, EventToken t) {
        return i->as<ProductStatics>()->purchased.remove(t);
    }
    static Vtable& vt() {
        static Vtable v = Vtable("IProductStatics", 15)
                              .set(S + 5, &CheckPrivilegeAsync)
                              .set(S + 6, &CheckPrivilegesAsync)
                              .set(S + 7, &add_ProductPurchased)
                              .set(S + 8, &remove_ProductPurchased);
        return v;
    }
    Iface istatics{vt(), this};
    ProductStatics() : Object(u"Windows.Xbox.ApplicationModel.Store.Product") { expose(IID_IProductStatics, &istatics); }
};

HRESULT get_product(const GUID& iid, void** out) {
    static auto* s = new ProductStatics();
    return s->query(iid, out);
}

}  // namespace

WINRT_REGISTER_CLASS(u"Windows.Xbox.ApplicationModel.Store.Product", get_product);

}  // namespace fl::winrt
