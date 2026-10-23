// SPDX-License-Identifier: MIT
// The one source that sees DXVK's internal headers (dxvk_bridge.h). DXVK's D3D11 device is created as its own
// d3d11.dll would create one (D3D11InternalCreateDevice, d3d11_main.cpp, which vgd3d11 does not link), on an
// instance whose Vulkan entry point is the Venus ICD's.
// DXVK's headers first: they enable the Win32 parts of the Vulkan headers before including them.
#include "d3d11/d3d11_device.h"
#include "d3d11/d3d11_interop.h"
#include "dxgi/dxgi_interfaces.h"
#include "dxvk/dxvk_instance.h"

#include "dxvk_bridge.h"
#include "log.h"

#include <cstring>
#include <mutex>

namespace dxbc_spv::dxbc {
// dxbc_container.cpp; not declared in its headers.
util::md5::Digest hashDxbcBinary(const void* data, size_t size);
} // namespace dxbc_spv::dxbc

namespace {

// Before DXVK's logger below (initialization follows definition order in a source): the logger reads its level
// when constructed. Without a configured log path DXVK would write <exe>_vgd3d11.log into the working directory of
// every process that loads us, DWM included; it logs only where one is configured.
const bool g_settings = (vgd::umd::dxvk::init_settings(), true);

} // namespace

namespace dxvk {
// What DXVK's d3d11.dll defines in d3d11_main.cpp.
Logger Logger::s_instance("vgd3d11.log");
} // namespace dxvk

namespace vgd::umd::dxvk {

using namespace ::dxvk;

namespace {

std::mutex g_lock;
Rc<DxvkInstance> g_instance;

// The process's DXVK instance on the Venus ICD, created on first use.
Rc<DxvkInstance> instance(PFN_vkGetInstanceProcAddr gipa)
{
    if (g_instance == nullptr) {
        DxvkInstanceImportInfo info = {};
        info.loaderProc = gipa;
        g_instance = new DxvkInstance(info, 0);
    }
    return g_instance;
}

// The GPU: DXVK lists its adapters best first, and Venus also offers the host's llvmpipe.
Rc<DxvkAdapter> gpu(const Rc<DxvkInstance>& inst)
{
    for (uint32_t i = 0; i < inst->adapterCount(); i++) {
        Rc<DxvkAdapter> a = inst->enumAdapters(i);
        if (a->info().deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU)
            return a;
    }
    return nullptr;
}

} // namespace

void init_settings()
{
    forward_settings({"DXVK_LOG_LEVEL", "DXVK_LOG_PATH", "DXVK_CONFIG", "DXVK_CONFIG_FILE", "DXVK_HUD",
                      "VN_DEBUG", "VN_PERF", "MESA_LOG_LEVEL"});
    if (GetEnvironmentVariableA("DXVK_LOG_PATH", nullptr, 0) == 0)
        SetEnvironmentVariableA("DXVK_LOG_PATH", "none");
}

HRESULT create_device(PFN_vkGetInstanceProcAddr gipa, D3D_FEATURE_LEVEL level, const char* const* extensions,
                      size_t extension_count, ID3D11Device5** out, DeviceHandles* handles)
{
    *out = nullptr;
    std::lock_guard g(g_lock);
    try {
        Rc<DxvkInstance> inst = instance(gipa);
        Rc<DxvkAdapter> adapter = gpu(inst);
        if (adapter == nullptr) {
            VGD_WARN("dxvk: no GPU adapter on the Venus ICD");
            return DXGI_ERROR_UNSUPPORTED;
        }
        // Only those the GPU has: DXVK fails device creation over any extension it asks for in vain.
        const Rc<vk::InstanceFn> vki = inst->vki();
        uint32_t count = 0;
        vki->vkEnumerateDeviceExtensionProperties(adapter->handle(), nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> available(count);
        vki->vkEnumerateDeviceExtensionProperties(adapter->handle(), nullptr, &count, available.data());
        DxvkExtensionList extra;
        for (size_t i = 0; i < extension_count; i++)
            for (const auto& e : available)
                if (!std::strcmp(e.extensionName, extensions[i]))
                    extra.push_back(vk::makeExtension(extensions[i]));
        adapter->enableExtensions(extra);

        Rc<DxvkDevice> device = adapter->createDevice();
        // At most 11_0: 11_1 promises target-independent rasterization (ForcedSampleCount with render targets
        // bound), which DXVK does not do: it rasterizes at the targets' sample count. DWM cuts the windows out of
        // their shadows with it at 11_1, and the shadows come out erased.
        const D3D_FEATURE_LEVEL max = std::min(D3D11Device::GetMaxFeatureLevel(*device), D3D_FEATURE_LEVEL_11_0);
        if (!level)
            level = max;
        if (level > max) {
            VGD_WARN("dxvk: feature level 0x%x asked, the device has 0x%x", level, max);
            return E_INVALIDARG;
        }
        // No DXGI adapter: this device is never handed to an application, and only its DXGI parent queries use it.
        Com<D3D11DXGIDevice> dxgi = new D3D11DXGIDevice(nullptr, nullptr, nullptr, inst, adapter, device, level, 0);
        HRESULT hr = dxgi->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(out));
        if (FAILED(hr))
            return hr;
        handles->instance = inst->vki()->instance();
        handles->physical = adapter->handle();
        handles->device = device->handle();
        std::strncpy(handles->name, adapter->info().deviceName, sizeof(handles->name) - 1);
        return S_OK;
    } catch (const DxvkError& e) {
        VGD_WARN("dxvk: device creation failed: %s", e.message().c_str());
        return E_FAIL;
    }
}

HRESULT texture_from_image(ID3D11Device5* device, const D3D11_TEXTURE2D_DESC1& desc, VkImage image,
                           ID3D11Texture2D** out)
{
    Com<IDXGIVkInteropDevice1> interop;
    HRESULT hr = device->QueryInterface(__uuidof(IDXGIVkInteropDevice1), reinterpret_cast<void**>(&interop));
    if (FAILED(hr))
        return hr;
    return interop->CreateTexture2DFromVkImage(&desc, image, out);
}

HRESULT image_info(ID3D11Device5* device, const D3D11_TEXTURE2D_DESC1& desc, VkFormat* format,
                   std::vector<VkFormat>* view_formats, VkImageUsageFlags* usage)
{
    // As D3D11CommonTexture sets up a texture of this description (d3d11_texture.cpp).
    auto* d3d11 = static_cast<D3D11Device*>(device);
    const DXGI_VK_FORMAT_MODE mode =
        (desc.BindFlags & D3D11_BIND_RENDER_TARGET) ? DXGI_VK_FORMAT_MODE_COLOR : DXGI_VK_FORMAT_MODE_ANY;
    const DXGI_VK_FORMAT_INFO info = d3d11->LookupFormat(desc.Format, mode);
    if (info.Format == VK_FORMAT_UNDEFINED)
        return E_INVALIDARG;
    DXGI_VK_FORMAT_FAMILY family = d3d11->LookupFamily(desc.Format, mode);
    if ((desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) &&
        (desc.Format == DXGI_FORMAT_R32_TYPELESS || desc.Format == DXGI_FORMAT_R32_FLOAT ||
         desc.Format == DXGI_FORMAT_R32_UINT || desc.Format == DXGI_FORMAT_R32_SINT)) {
        family.Add(info.Format);
        family.Add(VK_FORMAT_R32_SFLOAT);
        family.Add(VK_FORMAT_R32_UINT);
        family.Add(VK_FORMAT_R32_SINT);
    }
    *format = info.Format;
    view_formats->clear();
    if (family.FormatCount > 1)
        view_formats->assign(family.Formats, family.Formats + family.FormatCount);
    *usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE)
        *usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (desc.BindFlags & D3D11_BIND_RENDER_TARGET)
        *usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS)
        *usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    return S_OK;
}

void dxbc_checksum(const void* dxbc, size_t size, uint32_t checksum[4])
{
    const auto digest = dxbc_spv::dxbc::hashDxbcBinary(dxbc, size);
    std::memcpy(checksum, digest.data.data(), 16);
}

} // namespace vgd::umd::dxvk
