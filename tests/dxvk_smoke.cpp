// Checks that dxvk-native works on this machine, without the game:
// creates a D3D11 device, clears a render target on the GPU and reads one
// pixel back. Run: ./build/dxvk_smoke
//
// This file uses DXVK's Windows headers, so it must not include
// src/common/winabi.h (both define GUID, HRESULT...).
#include <d3d11.h>
#include <dxgi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main() {
    // dxvk-native refuses to start without a WSI choice.
    setenv("DXVK_WSI_DRIVER", "SDL3", 0);

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &device, &level, &ctx);
    if (FAILED(hr)) {
        std::fprintf(stderr, "FAIL: D3D11CreateDevice hr=0x%08x\n", unsigned(hr));
        return 1;
    }

    IDXGIDevice* dxgi_device = nullptr;
    IDXGIAdapter* adapter = nullptr;
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_device))) &&
        SUCCEEDED(dxgi_device->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc))) {
        char name[128];
        size_t i = 0;
        for (; i < sizeof(name) - 1 && desc.Description[i]; i++) name[i] = char(desc.Description[i]);
        name[i] = 0;
        std::printf("adapter: %s, feature level 0x%x, VRAM %zu MB\n", name, unsigned(level),
                    size_t(desc.DedicatedVideoMemory >> 20));
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = 64;
    td.Height = 64;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* target = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &target)) ||
        FAILED(device->CreateRenderTargetView(target, nullptr, &rtv))) {
        std::fprintf(stderr, "FAIL: render target creation\n");
        return 1;
    }

    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &staging))) {
        std::fprintf(stderr, "FAIL: staging texture creation\n");
        return 1;
    }

    const float colour[4] = {1.0f, 0.5f, 0.0f, 1.0f};
    ctx->ClearRenderTargetView(rtv, colour);
    ctx->CopyResource(staging, target);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        std::fprintf(stderr, "FAIL: Map\n");
        return 1;
    }
    uint8_t px[4];
    std::memcpy(px, mapped.pData, 4);
    ctx->Unmap(staging, 0);

    std::printf("pixel: %u %u %u %u\n", px[0], px[1], px[2], px[3]);
    // 0.5 * 255 rounds to 127 or 128 depending on the driver.
    bool ok = px[0] == 255 && (px[1] == 127 || px[1] == 128) && px[2] == 0 && px[3] == 255;

    staging->Release();
    rtv->Release();
    target->Release();
    if (adapter) adapter->Release();
    if (dxgi_device) dxgi_device->Release();
    ctx->Release();
    device->Release();

    std::printf("%s\n", ok ? "OK: dxvk-native works" : "FAIL: wrong pixel colour");
    return ok ? 0 : 1;
}
