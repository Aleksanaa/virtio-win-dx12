// SPDX-License-Identifier: MIT
// Direct3D 9 tests: which driver D3D9 runs on and whether it renders. Each prints the adapter identifier and caps and
// the graphics modules the process loaded (a D3D9 UMD, or d3d9on12.dll on the D3D12 UMD), then clears and draws a
// fixed-function triangle on a HAL device, reads it back and checks it.
//   hal      through the adapter's own D3D9 driver
//   9on12    through D3D9On12, opted in with Direct3DCreate9On12Ex as an app can, whatever the adapter's D3D9 UMD is
//   present  the window shown and the frame presented for a second (60 presents), which must all succeed
#include "tests.h"

#include <d3d9.h>
#include <psapi.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {

FILE* out = stdout;

// From the SDK's d3d9on12.h, which MinGW lacks.
struct D3D9On12Args {
    BOOL Enable9On12;
    IUnknown* pD3D12Device;
    IUnknown* ppD3D12Queues[2];
    UINT NumQueues;
    UINT NodeMask;
};
using PfnDirect3DCreate9On12Ex = HRESULT(WINAPI*)(UINT, D3D9On12Args*, UINT, IDirect3D9Ex**);

// The modules whose names say which driver stack is in use.
void modules(const char* when)
{
    HMODULE mods[1024];
    DWORD needed = 0;
    EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed);
    std::fprintf(out, "modules %s:", when);
    for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 1024; i++) {
        char path[MAX_PATH];
        if (!GetModuleFileNameA(mods[i], path, sizeof(path)))
            continue;
        char lower[MAX_PATH];
        size_t n = 0;
        for (; path[n] && n + 1 < sizeof(lower); n++)
            lower[n] = static_cast<char>(std::tolower(static_cast<unsigned char>(path[n])));
        lower[n] = 0;
        for (const char* key : {"d3d9", "d3d1", "vgd", "viogpu", "vulkan", "dxgi", "warp"}) {
            if (std::strstr(lower, key)) {
                std::fprintf(out, " %s", path);
                break;
            }
        }
    }
    std::fprintf(out, "\n");
}

struct Vertex {
    float x, y, z, rhw;
    D3DCOLOR color;
};

// The pixel at (x, y) of the device's back buffer, through a system-memory copy.
bool pixel(IDirect3DDevice9Ex* dev, UINT x, UINT y, D3DCOLOR* value)
{
    IDirect3DSurface9 *back = nullptr, *sys = nullptr;
    HRESULT hr = dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
    D3DSURFACE_DESC desc{};
    if (SUCCEEDED(hr))
        hr = back->GetDesc(&desc);
    if (SUCCEEDED(hr))
        hr = dev->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    if (SUCCEEDED(hr))
        hr = dev->GetRenderTargetData(back, sys);
    D3DLOCKED_RECT lr{};
    if (SUCCEEDED(hr))
        hr = sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
    if (SUCCEEDED(hr)) {
        *value = static_cast<const D3DCOLOR*>(lr.pBits)[y * (lr.Pitch / 4) + x] & 0x00ffffff;
        sys->UnlockRect();
    } else {
        std::fprintf(out, "readback failed: 0x%08lx\n", hr);
    }
    if (sys)
        sys->Release();
    if (back)
        back->Release();
    return SUCCEEDED(hr);
}

bool run(bool on12, bool show)
{
    modules("at start");

    IDirect3D9Ex* d3d = nullptr;
    HRESULT hr;
    if (on12) {
        auto create = reinterpret_cast<PfnDirect3DCreate9On12Ex>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("d3d9.dll"), "Direct3DCreate9On12Ex")));
        D3D9On12Args args{};
        args.Enable9On12 = TRUE;
        hr = create ? create(D3D_SDK_VERSION, &args, 1, &d3d) : E_NOTIMPL;
        std::fprintf(out, "Direct3DCreate9On12Ex: 0x%08lx\n", hr);
    } else {
        hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d);
        std::fprintf(out, "Direct3DCreate9Ex: 0x%08lx\n", hr);
    }
    if (FAILED(hr))
        return false;
    D3DADAPTER_IDENTIFIER9 id{};
    hr = d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);
    std::fprintf(out, "adapter: 0x%08lx driver '%s' description '%s' version %u.%u.%u.%u vendor %04lx device %04lx\n",
                 hr, id.Driver, id.Description, HIWORD(id.DriverVersion.HighPart), LOWORD(id.DriverVersion.HighPart),
                 HIWORD(id.DriverVersion.LowPart), LOWORD(id.DriverVersion.LowPart), id.VendorId, id.DeviceId);
    D3DCAPS9 caps{};
    hr = d3d->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);
    std::fprintf(out, "HAL caps: 0x%08lx vs %lx ps %lx max texture %lux%lu\n", hr, caps.VertexShaderVersion & 0xffff,
                 caps.PixelShaderVersion & 0xffff, caps.MaxTextureWidth, caps.MaxTextureHeight);
    modules("after the caps query");

    const UINT size = 64;
    const int left = 200, top = 200;
    HWND wnd = CreateWindowExA(WS_EX_TOPMOST, "STATIC", "d3d9probe", WS_POPUP, left, top, size, size, nullptr, nullptr,
                               nullptr, nullptr);
    if (show)
        ShowWindow(wnd, SW_SHOWNOACTIVATE);
    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth = size;
    pp.BackBufferHeight = size;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = wnd;
    pp.Windowed = TRUE;
    IDirect3DDevice9Ex* dev = nullptr;
    hr = d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, nullptr,
                             &dev);
    std::fprintf(out, "CreateDeviceEx (hardware vertex processing): 0x%08lx\n", hr);
    if (FAILED(hr)) {
        hr = d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp,
                                 nullptr, &dev);
        std::fprintf(out, "CreateDeviceEx (software vertex processing): 0x%08lx\n", hr);
    }
    modules("after CreateDeviceEx");
    if (FAILED(hr)) {
        std::fprintf(out, "result: FAIL (no HAL device)\n");
        return false;
    }

    // Clear to blue, then a red triangle over the middle: the middle turns red, the corner stays blue.
    const D3DCOLOR clear = D3DCOLOR_XRGB(0x20, 0x40, 0xc0), red = D3DCOLOR_XRGB(0xff, 0x00, 0x00);
    const Vertex tri[3] = {
        {size * 0.5f, size * 0.1f, 0.5f, 1.0f, red},
        {size * 0.9f, size * 0.9f, 0.5f, 1.0f, red},
        {size * 0.1f, size * 0.9f, 0.5f, 1.0f, red},
    };
    hr = dev->Clear(0, nullptr, D3DCLEAR_TARGET, clear, 1.0f, 0);
    std::fprintf(out, "Clear: 0x%08lx\n", hr);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    hr = dev->BeginScene();
    if (SUCCEEDED(hr))
        hr = dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(Vertex));
    if (SUCCEEDED(hr))
        hr = dev->EndScene();
    std::fprintf(out, "draw: 0x%08lx\n", hr);

    D3DCOLOR corner = 0, middle = 0;
    const bool read = pixel(dev, 2, 2, &corner) && pixel(dev, size / 2, size / 2, &middle);
    std::fprintf(out, "pixels: corner %06lx (want %06lx), middle %06lx (want %06lx)\n", corner, clear & 0xffffff,
                 middle, red & 0xffffff);
    hr = dev->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
    std::fprintf(out, "PresentEx: 0x%08lx\n", hr);
    bool pass = read && corner == (clear & 0xffffff) && middle == (red & 0xffffff);
    if (show) {
        HRESULT last = S_OK;
        for (int frame = 0; frame < 60 && SUCCEEDED(last); frame++) {
            dev->Clear(0, nullptr, D3DCLEAR_TARGET, clear, 1.0f, 0);
            dev->BeginScene();
            dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(Vertex));
            dev->EndScene();
            last = dev->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
            MSG msg;
            while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
                DispatchMessageA(&msg);
            Sleep(16);
        }
        // Device state after the presents: a failed kernel present removes the device.
        const HRESULT state = dev->CheckDeviceState(wnd);
        std::fprintf(out, "60 presents: last 0x%08lx, device state 0x%08lx\n", last, state);
        pass = pass && SUCCEEDED(last) && SUCCEEDED(state);
    }
    std::fprintf(out, "result: %s\n", pass ? "PASS" : "FAIL");
    dev->Release();
    d3d->Release();
    DestroyWindow(wnd);
    return pass;
}

} // namespace

namespace panel {

TestGroup d3d9Tests()
{
    return {"d3d9", L"Direct3D 9", L"Through D3D9On12 on vgd's D3D12 driver", {
        {"hal", L"Device", L"A clear and a triangle on the adapter's D3D9 device", [] { return run(false, false); }},
        {"9on12", L"D3D9On12", L"The same with D3D9On12 asked for, as an application can",
         [] { return run(true, false); }},
        {"present", L"Presents", L"Shows a small window and presents 60 frames", [] { return run(false, true); }},
    }};
}

}  // namespace panel
