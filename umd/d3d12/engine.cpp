// SPDX-License-Identifier: MIT
#include "engine.h"
#include "log.h"
#include "vkd3d_bridge.h"
#include "vulkan_icd.h"


namespace vgd::umd {

namespace {

std::mutex g_lock;
std::weak_ptr<Engine> g_engine;

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
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    hr = vgd_vkd3d_vulkan_handles(e->device.get(), &instance, &physical, &device);
    if (SUCCEEDED(hr))
        hr = e->vulkan.init(gipa, instance, physical, device);
    if (FAILED(hr))
        return hr;
    VGD_TRACE("engine: vkd3d-proton on %s (external memory %d, drm modifiers %d)", name, e->vulkan.external_memory,
              e->vulkan.drm_modifier);
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
        vulkan.vk.vkDestroyFence(vulkan.device, fence_, nullptr);
    if (pool_)
        vulkan.vk.vkDestroyCommandPool(vulkan.device, pool_, nullptr);
}

HRESULT Engine::run(const std::function<void(VkCommandBuffer)>& record)
{
    const auto& vk = vulkan.vk;
    const VkDevice vk_device = vulkan.device;
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
