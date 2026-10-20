// SPDX-License-Identifier: MIT
// Guest probe: lists DXGI adapters and tries D3D12CreateDevice on each. Plain-text output for the harness.
#include <windows.h>
#include <dxgi1_4.h>
#include <directx/d3d12.h>
#include <dxguids/dxguids.h>

#include <cstdio>

int main()
{
    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        std::printf("CreateDXGIFactory1 failed: 0x%08lx\n", hr);
        return 1;
    }
    int failures = 0;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        std::printf("adapter %u: %ls (vendor 0x%04x device 0x%04x subsys 0x%08x flags 0x%x)\n", i, desc.Description,
                    desc.VendorId, desc.DeviceId, desc.SubSysId, desc.Flags);

        ID3D12Device* device = nullptr;
        hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        std::printf("  D3D12CreateDevice(FL 11_0): 0x%08lx %s\n", hr, SUCCEEDED(hr) ? "PASS" : "FAIL");
        if (device) {
            D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_0};
            device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));
            std::printf("  highest shader model: 0x%x\n", sm.HighestShaderModel);
            device->Release();
        } else if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            failures++;
        }
        adapter->Release();
    }
    factory->Release();
    return failures ? 1 : 0;
}
