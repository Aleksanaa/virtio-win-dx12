// SPDX-License-Identifier: MIT
// The D3D12 implementation behind the DDI: vkd3d-proton's ID3D12Device on the Venus ICD, one per process. The
// adapter holds it for caps, every DDI device forwards to it. Besides D3D12, it gives Vulkan access to the same
// VkDevice for what the D3D12 API cannot express: images on memory shared with other processes (shared_image.cpp).
#pragma once

#include "com.h"
#include "wdk.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vgd::umd {

struct Engine {
    Com<ID3D12Device> device;
    std::string name;   // the Vulkan physical device's

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice vk_device = VK_NULL_HANDLE;
    // Every queue family of the device: images we create are shared concurrently by all, as vkd3d-proton's are.
    std::vector<uint32_t> families;
    // VK_KHR_external_memory_fd + VK_EXT_external_memory_dma_buf (our ICD exports NT handles through them), and
    // VK_EXT_image_drm_format_modifier, for images shared with other processes.
    bool external_memory = false;
    bool drm_modifier = false;

    struct Vk {
#define VGD_VK_FN(name) PFN_##name name = nullptr;
#include "engine_vk.inc"
#undef VGD_VK_FN
    } vk;

    // Records commands with `record` and runs them on a queue of the device, waiting for completion.
    HRESULT run(const std::function<void(VkCommandBuffer)>& record);

    ~Engine();

    // The process's engine, created on first use.
    static HRESULT get(std::shared_ptr<Engine>* out);

private:
    std::mutex lock_;
    Com<ID3D12CommandQueue> queue_;   // for run(): a vkd3d-proton queue, borrowed through its interop interface
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
};

} // namespace vgd::umd
