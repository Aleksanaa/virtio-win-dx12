// SPDX-License-Identifier: MIT
// The D3D12 implementation behind the DDI: vkd3d-proton's ID3D12Device on the Venus ICD, one per process. The
// adapter holds it for caps, every DDI device forwards to it. Besides D3D12, it gives Vulkan access to the same
// VkDevice for what the D3D12 API cannot express: images on memory shared with other processes (shared_image.h).
#pragma once

#include "com.h"
#include "vulkan_device.h"
#include "wdk.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace vgd::umd {

struct Engine {
    Com<ID3D12Device> device;
    std::string name;   // the Vulkan physical device's
    VulkanDevice vulkan;

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
