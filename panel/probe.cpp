// SPDX-License-Identifier: MIT
// The graphics probe: `vgd-panel.exe /probe` tries each API on the VirtIO GPU as an application would (Vulkan through
// the loader, D3D12 and D3D11 on the adapter, OpenGL on a window) and prints key=value lines. Each API first prints
// "<api>.status=checking", so one that crashes the process is told apart from one that was never reached. The panel
// runs it as a child process (startProbe) and reads its output.
#include "panel.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dxgi1_4.h>
#include <directx/d3d12.h>
#include <d3d11.h>
#include <dxguids/dxguids.h>
#include <GL/gl.h>

#include <cstdio>
#include <thread>

namespace panel {

namespace {

void out(const std::string& key, const std::string& value)
{
    std::string line = key + "=" + value + "\n";
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.data(), line.size(), &written, nullptr);
}

std::string hex(HRESULT hr)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "error 0x%08lx", static_cast<unsigned long>(hr));
    return buf;
}

void probeVulkan()
{
    out("vk.status", "checking");
    HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
    if (!loader) {
        out("vk.status", "no loader");
        return;
    }
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "vgd-panel";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance instance;
    if (VkResult r = createInstance(&ci, nullptr, &instance)) {
        out("vk.status", "error " + std::to_string(r));
        return;
    }
    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
    auto properties2 =
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(gipa(instance, "vkGetPhysicalDeviceProperties2"));
    auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));
    uint32_t n = 0;
    enumerate(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devices(n);
    enumerate(instance, &n, devices.data());
    bool found = false;
    for (VkPhysicalDevice d : devices) {
        VkPhysicalDeviceDriverProperties driver = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
        properties2(d, &p);
        if (driver.driverID != VK_DRIVER_ID_MESA_VENUS)
            continue;
        uint32_t v = p.properties.apiVersion;
        out("vk.device", p.properties.deviceName);
        out("vk.api", std::to_string(VK_API_VERSION_MAJOR(v)) + "." + std::to_string(VK_API_VERSION_MINOR(v)) + "." +
                          std::to_string(VK_API_VERSION_PATCH(v)));
        out("vk.driver", driver.driverName);
        out("vk.driver_info", driver.driverInfo);
        found = true;
        break;
    }
    destroy(instance, nullptr);
    out("vk.status", found ? "ok" : "no device");
}

// The VirtIO GPU among the DXGI adapters.
IDXGIAdapter1* findAdapter()
{
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return nullptr;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc;
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && (desc.VendorId == 0x1af4 || wcsstr(desc.Description, L"VirtIO")))
            break;
        adapter->Release();
        adapter = nullptr;
    }
    factory->Release();
    return adapter;
}

std::string level(UINT fl) { return std::to_string(fl >> 12) + "_" + std::to_string((fl >> 8) & 0xf); }

void probeD3D12(IDXGIAdapter1* adapter)
{
    out("d3d12.status", "checking");
    ID3D12Device* device = nullptr;
    HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr)) {
        out("d3d12.status", hex(hr));
        return;
    }
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                                  D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl = {ARRAYSIZE(levels), levels};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl))))
        out("d3d12.level", level(fl.MaxSupportedFeatureLevel));
    // The runtime refuses a shader model newer than it knows: ask from the newest down.
    for (int sm = D3D_HIGHEST_SHADER_MODEL; sm >= D3D_SHADER_MODEL_5_1; sm = sm == 0x60 ? 0x51 : sm - 1) {
        D3D12_FEATURE_DATA_SHADER_MODEL data = {static_cast<D3D_SHADER_MODEL>(sm)};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &data, sizeof(data)))) {
            out("d3d12.shader_model",
                std::to_string(data.HighestShaderModel >> 4) + "." + std::to_string(data.HighestShaderModel & 0xf));
            break;
        }
    }
    device->Release();
    out("d3d12.status", "ok");
}

void probeD3D11(IDXGIAdapter1* adapter)
{
    out("d3d11.status", "checking");
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                  D3D_FEATURE_LEVEL_10_0};
    ID3D11Device* device = nullptr;
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, ARRAYSIZE(levels),
                                   D3D11_SDK_VERSION, &device, &got, nullptr);
    if (FAILED(hr)) {
        out("d3d11.status", hex(hr));
        return;
    }
    out("d3d11.level", level(got));
    device->Release();
    out("d3d11.status", "ok");
}

// OpenGL as an application gets it: a pixel format on a window on the VirtIO GPU's display, through opengl32.dll.
void probeOpenGL()
{
    out("gl.status", "checking");
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"vgd-panel-probe";
    RegisterClassW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                                wc.hInstance, nullptr);
    HDC dc = GetDC(window);
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1};
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    int pf = ChoosePixelFormat(dc, &pfd);
    HGLRC context = pf && SetPixelFormat(dc, pf, &pfd) ? wglCreateContext(dc) : nullptr;
    if (!context || !wglMakeCurrent(dc, context)) {
        out("gl.status", "error " + std::to_string(GetLastError()));
    } else {
        auto str = [](GLenum e) { auto s = glGetString(e); return s ? std::string(reinterpret_cast<const char*>(s)) : ""; };
        out("gl.version", str(GL_VERSION));
        out("gl.renderer", str(GL_RENDERER));
        out("gl.vendor", str(GL_VENDOR));
        wglMakeCurrent(nullptr, nullptr);
        out("gl.status", "ok");
    }
    if (context)
        wglDeleteContext(context);
    ReleaseDC(window, dc);
    DestroyWindow(window);
}

}  // namespace

int runProbe()
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    probeVulkan();
    IDXGIAdapter1* adapter = findAdapter();
    if (adapter) {
        probeD3D12(adapter);
        probeD3D11(adapter);
        adapter->Release();
    } else {
        out("d3d12.status", "no adapter");
        out("d3d11.status", "no adapter");
    }
    probeOpenGL();
    out("done", "1");
    return 0;
}

ProbeResult probeNow(DWORD timeoutMs)
{
    ProbeResult r;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(self) + L"\" /probe";
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (CreatePipe(&read, &write, &sa, 1 << 16)) {
        SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = write;
        si.hStdError = write;
        if (!CreateProcessW(self, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                            &pi))
            r["failed"] = L"1";
        CloseHandle(write);
    }
    if (pi.hProcess) {
        // The output fits the pipe's buffer, so the probe never waits on us: wait for it, then read.
        if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_TIMEOUT) {
            TerminateProcess(pi.hProcess, 1);
            r["timeout"] = L"1";
        }
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        char hexCode[16];
        snprintf(hexCode, sizeof(hexCode), "0x%08lx", code);
        r["exit"] = widen(hexCode);
        std::string text;
        char buf[4096];
        DWORD got;
        while (ReadFile(read, buf, sizeof(buf), &got, nullptr) && got)
            text.append(buf, got);
        for (size_t start = 0; start < text.size();) {
            size_t end = text.find('\n', start);
            if (end == std::string::npos)
                end = text.size();
            std::string line = text.substr(start, end - start);
            size_t eq = line.find('=');
            if (eq != std::string::npos)
                r[line.substr(0, eq)] = widen(line.substr(eq + 1));
            start = end + 1;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    if (read)
        CloseHandle(read);
    return r;
}

void startProbe(HWND window, UINT msg, DWORD timeoutMs)
{
    std::thread([=] {
        auto* result = new ProbeResult(probeNow(timeoutMs));
        if (!PostMessageW(window, msg, 0, reinterpret_cast<LPARAM>(result)))
            delete result;
    }).detach();
}

}  // namespace panel
