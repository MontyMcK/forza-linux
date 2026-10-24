// Windows.Foundation.Collections.IVectorView<T> for interface-typed T.
// Slots (pinterface layout, confirmed by the proxy data, e.g. IVectorView<User>):
//   S+0 GetAt(uint32, T*), S+1 get_Size(uint32*), S+2 IndexOf(T, uint32*, bool*),
//   S+3 GetMany(start, capacity, T* items, uint32* count)
#pragma once

#include <vector>

#include "hle/winrt/object.h"

namespace fl::winrt {

struct VectorView : Object {
    // `items` are interface pointers the view takes ownership of (already AddRef'd).
    VectorView(const GUID& iid, std::vector<void*> items);
    ~VectorView() override;
    Iface iview;
    std::vector<void*> items;
};

}  // namespace fl::winrt
