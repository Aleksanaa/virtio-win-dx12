// SPDX-License-Identifier: MIT
#include "engine.h"
#include "log.h"
#include "vkd3d_bridge.h"
#include "vulkan_icd.h"

#include <cstring>

namespace vgd::umd {

namespace {

std::mutex g_lock;
std::weak_ptr<Engine> g_engine;

// Device extensions vkd3d-proton has no use for, enabled for images shared with other processes when available.
constexpr const char* kSharingExtensions[] = {
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
};

bool has_extension(const std::vector<VkExtensionProperties>& exts, const char* name)
{
    for (const auto& e : exts)
        if (!std::strcmp(e.extensionName, name))
            return true;
    return false;
}

HRESULT create(std::shared_ptr<Engine>* out)
{
    PFN_vkGetInstanceProcAddr gipa = load_venus_icd();
    if (!gipa)
        return DXGI_ERROR_UNSUPPORTED;
    auto e = std::make_shared<Engine>();
    char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = {};
    HRESULT hr = vgd_vkd3d_create_device(gipa, kSharingExtensions, ARRAYSIZE(kSharingExtensions), e->device.put_void(),
                                         name, sizeof(name));
    if (FAILED(hr)) {
        VGD_WARN("engine: vkd3d-proton device on the Venus ICD failed: 0x%08lx", hr);
        return hr;
    }
    e->name = name;
    hr = vgd_vkd3d_vulkan_handles(e->device.get(), &e->instance, &e->physical, &e->vk_device);
    if (FAILED(hr))
        return hr;

#define VGD_VK_FN(fn) e->vk.fn = reinterpret_cast<PFN_##fn>(gipa(e->instance, #fn));
#include "engine_vk.inc"
#undef VGD_VK_FN
    // Device-level functions straight from the device, skipping the instance's trampolines.
#define VGD_VK_FN(fn)                                                                                       \
    if (auto p = e->vk.vkGetDeviceProcAddr(e->vk_device, #fn))                                              \
        e->vk.fn = reinterpret_cast<PFN_##fn>(p);
#include "engine_vk.inc"
#undef VGD_VK_FN

    uint32_t count = 0;
    e->vk.vkGetPhysicalDeviceQueueFamilyProperties(e->physical, &count, nullptr);
    for (uint32_t i = 0; i < count; i++)
        e->families.push_back(i);
    e->vk.vkEnumerateDeviceExtensionProperties(e->physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    e->vk.vkEnumerateDeviceExtensionProperties(e->physical, nullptr, &count, exts.data());
    e->external_memory = has_extension(exts, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) &&
                         has_extension(exts, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) &&
                         e->vk.vkGetMemoryFdKHR && e->vk.vkGetMemoryFdPropertiesKHR;
    e->drm_modifier = e->external_memory && has_extension(exts, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    VGD_TRACE("engine: vkd3d-proton on %s (external memory %d, drm modifiers %d)", name, e->external_memory,
              e->drm_modifier);
    *out = std::move(e);
    return S_OK;
}

} // namespace

HRESULT Engine::get(std::shared_ptr<Engine>* out)
{
    std::lock_guard g(g_lock);
    if ((*out = g_engine.lock()))
        return S_OK;
    HRESULT hr = create(out);
    if (SUCCEEDED(hr))
        g_engine = *out;
    return hr;
}

Engine::~Engine()
{
    if (fence_)
        vk.vkDestroyFence(vk_device, fence_, nullptr);
    if (pool_)
        vk.vkDestroyCommandPool(vk_device, pool_, nullptr);
}

HRESULT Engine::run(const std::function<void(VkCommandBuffer)>& record)
{
    std::lock_guard g(lock_);
    if (!queue_) {
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        HRESULT hr = device->CreateCommandQueue(&desc, __uuidof(ID3D12CommandQueue), queue_.put_void());
        if (FAILED(hr))
            return hr;
    }
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    HRESULT hr = vgd_vkd3d_lock_queue(device.get(), queue_.get(), &queue, &family);
    if (FAILED(hr))
        return hr;
    VkResult r = VK_SUCCESS;
    if (!pool_) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = family;
        r = vk.vkCreateCommandPool(vk_device, &pci, nullptr, &pool_);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (r == VK_SUCCESS)
            r = vk.vkCreateFence(vk_device, &fci, nullptr, &fence_);
    }
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (r == VK_SUCCESS) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        r = vk.vkAllocateCommandBuffers(vk_device, &ai, &cmd);
    }
    if (r == VK_SUCCESS) {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk.vkBeginCommandBuffer(cmd, &bi);
        record(cmd);
        r = vk.vkEndCommandBuffer(cmd);
    }
    if (r == VK_SUCCESS) {
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        r = vk.vkQueueSubmit(queue, 1, &si, fence_);
    }
    if (r == VK_SUCCESS)
        r = vk.vkWaitForFences(vk_device, 1, &fence_, VK_TRUE, UINT64_MAX);
    vgd_vkd3d_unlock_queue(device.get(), queue_.get());
    if (fence_)
        vk.vkResetFences(vk_device, 1, &fence_);
    if (pool_)
        vk.vkResetCommandPool(vk_device, pool_, 0);
    if (r != VK_SUCCESS) {
        VGD_WARN("engine: one-off submission failed: %d", r);
        return E_FAIL;
    }
    return S_OK;
}

} // namespace vgd::umd
