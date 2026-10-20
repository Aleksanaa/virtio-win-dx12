// SPDX-License-Identifier: MIT
// Guest probe: lists DXGI adapters and tries D3D12CreateDevice on each. Plain-text output for the harness.
//   --debug  enable the D3D12 debug layer (needs the "Graphics Tools" optional feature) and print the runtime's
//            messages after each attempt.
#include <windows.h>
#include <dxgi1_4.h>
#include <dxgidebug.h>
#include <directx/d3d12.h>
#include <dxguids/dxguids.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

IDXGIInfoQueue* g_info = nullptr;

void enable_debug_layer()
{
    ID3D12Debug* debug = nullptr;
    HRESULT hr = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
    if (FAILED(hr)) {
        std::printf("debug layer unavailable (0x%08lx): install Graphics Tools, see vgd-test.ps1\n", hr);
        return;
    }
    debug->EnableDebugLayer();
    debug->Release();

    using GetDebugInterface1 = HRESULT(WINAPI*)(UINT, REFIID, void**);
    auto get = reinterpret_cast<GetDebugInterface1>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("dxgi.dll"), "DXGIGetDebugInterface1")));
    if (get && SUCCEEDED(get(0, IID_IDXGIInfoQueue, reinterpret_cast<void**>(&g_info))))
        std::printf("debug layer on; runtime messages follow each attempt\n");
    else
        std::printf("debug layer on; messages only in the debugger output (DebugView)\n");
}

void print_messages()
{
    if (!g_info)
        return;
    const UINT64 n = g_info->GetNumStoredMessages(DXGI_DEBUG_ALL);
    for (UINT64 i = 0; i < n; i++) {
        SIZE_T len = 0;
        g_info->GetMessage(DXGI_DEBUG_ALL, i, nullptr, &len);
        std::vector<char> buf(len);
        auto* m = reinterpret_cast<DXGI_INFO_QUEUE_MESSAGE*>(buf.data());
        if (SUCCEEDED(g_info->GetMessage(DXGI_DEBUG_ALL, i, m, &len)))
            std::printf("  [runtime] %.*s\n", static_cast<int>(m->DescriptionByteLength), m->pDescription);
    }
    g_info->ClearStoredMessages(DXGI_DEBUG_ALL);
}

} // namespace

int main(int argc, char** argv)
{
    std::printf("d3dprobe built " __DATE__ " " __TIME__ ", args:");
    for (int i = 1; i < argc; i++)
        std::printf(" %s", argv[i]);
    std::printf("\n");
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--debug"))
            enable_debug_layer();

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
        print_messages();
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
