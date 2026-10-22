// SPDX-License-Identifier: MIT
// Prints what the installed D3D10/11 UMD reports through the runtime: the adapter, the feature level, every
// CheckFeatureSupport query, and per format its support flags and MSAA quality levels. Run on two UMDs and diff
// the outputs to find an answer one gives and the other does not (DWM drops effects silently over these).
//   d3d11caps [output file]
#include <d3d11_4.h>
#include <dxgi1_6.h>

#include <cstdio>

namespace {

FILE* out = stdout;

template <typename T>
void feature(ID3D11Device* d, D3D11_FEATURE f, const char* name, T data = {})
{
    HRESULT hr = d->CheckFeatureSupport(f, &data, sizeof(data));
    std::fprintf(out, "feature %s: 0x%08lx", name, hr);
    const unsigned char* b = reinterpret_cast<const unsigned char*>(&data);
    for (size_t i = 0; SUCCEEDED(hr) && i < sizeof(data); i += 4) {
        unsigned v = 0;
        for (size_t j = 0; j < 4 && i + j < sizeof(data); j++)
            v |= unsigned(b[i + j]) << (8 * j);
        std::fprintf(out, " %08x", v);
    }
    std::fprintf(out, "\n");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc > 1 && !(out = std::fopen(argv[1], "w"))) {
        std::fprintf(stderr, "cannot write %s\n", argv[1]);
        return 1;
    }
    ID3D11Device* d = nullptr;
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   nullptr, 0, D3D11_SDK_VERSION, &d, &level, nullptr);
    if (FAILED(hr)) {
        std::fprintf(out, "D3D11CreateDevice: 0x%08lx\n", hr);
        return 1;
    }
    IDXGIDevice* dxgi = nullptr;
    IDXGIAdapter* adapter = nullptr;
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(d->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi))) &&
        SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc)))
        std::fprintf(out, "adapter %ls vendor %04x device %04x video %zu MB shared %zu MB\n", desc.Description,
                     desc.VendorId, desc.DeviceId, desc.DedicatedVideoMemory >> 20, desc.SharedSystemMemory >> 20);
    std::fprintf(out, "feature level 0x%x\n", level);

#define F(f, T) feature<T>(d, f, #f)
    F(D3D11_FEATURE_THREADING, D3D11_FEATURE_DATA_THREADING);
    F(D3D11_FEATURE_DOUBLES, D3D11_FEATURE_DATA_DOUBLES);
    F(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS, D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS);
    F(D3D11_FEATURE_D3D11_OPTIONS, D3D11_FEATURE_DATA_D3D11_OPTIONS);
    F(D3D11_FEATURE_ARCHITECTURE_INFO, D3D11_FEATURE_DATA_ARCHITECTURE_INFO);
    F(D3D11_FEATURE_D3D9_OPTIONS, D3D11_FEATURE_DATA_D3D9_OPTIONS);
    F(D3D11_FEATURE_SHADER_MIN_PRECISION_SUPPORT, D3D11_FEATURE_DATA_SHADER_MIN_PRECISION_SUPPORT);
    F(D3D11_FEATURE_D3D9_SHADOW_SUPPORT, D3D11_FEATURE_DATA_D3D9_SHADOW_SUPPORT);
    F(D3D11_FEATURE_D3D11_OPTIONS1, D3D11_FEATURE_DATA_D3D11_OPTIONS1);
    F(D3D11_FEATURE_D3D9_SIMPLE_INSTANCING_SUPPORT, D3D11_FEATURE_DATA_D3D9_SIMPLE_INSTANCING_SUPPORT);
    F(D3D11_FEATURE_MARKER_SUPPORT, D3D11_FEATURE_DATA_MARKER_SUPPORT);
    F(D3D11_FEATURE_D3D9_OPTIONS1, D3D11_FEATURE_DATA_D3D9_OPTIONS1);
    F(D3D11_FEATURE_D3D11_OPTIONS2, D3D11_FEATURE_DATA_D3D11_OPTIONS2);
    F(D3D11_FEATURE_D3D11_OPTIONS3, D3D11_FEATURE_DATA_D3D11_OPTIONS3);
    F(D3D11_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, D3D11_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT);
    F(D3D11_FEATURE_D3D11_OPTIONS4, D3D11_FEATURE_DATA_D3D11_OPTIONS4);
    F(D3D11_FEATURE_SHADER_CACHE, D3D11_FEATURE_DATA_SHADER_CACHE);
    F(D3D11_FEATURE_D3D11_OPTIONS5, D3D11_FEATURE_DATA_D3D11_OPTIONS5);
#undef F

    for (UINT f = 1; f <= 191; f++) {
        const DXGI_FORMAT format = static_cast<DXGI_FORMAT>(f);
        UINT support = 0;
        if (FAILED(d->CheckFormatSupport(format, &support)) || !support)
            continue;
        D3D11_FEATURE_DATA_FORMAT_SUPPORT2 s2{format, 0};
        if (FAILED(d->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &s2, sizeof(s2))))
            s2.OutFormatSupport2 = 0;
        UINT q[3] = {};
        for (UINT i = 0; i < 3; i++)
            if (FAILED(d->CheckMultisampleQualityLevels(format, 2u << i, &q[i])))
                q[i] = ~0u;
        std::fprintf(out, "format %3u: support %08x support2 %08x msaa2 %u msaa4 %u msaa8 %u\n", f, support,
                     s2.OutFormatSupport2, q[0], q[1], q[2]);
    }
    if (out != stdout)
        std::fclose(out);
    return 0;
}
