// SPDX-License-Identifier: MIT
// The Vulkan device behind an engine (vkd3d-proton's or DXVK's, both on the Venus ICD), as the UMDs use it directly:
// for images on memory shared with other processes (linear_image.h).
#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <windows.h>

#include <vector>

namespace vgd::umd {

// Device extensions the engines have no use for, which the UMDs ask them to enable (when available) for images
// shared with other processes.
extern const char* const kSharingExtensions[3];

struct VulkanDevice {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    // Every queue family of the device: shared images are used concurrently by all, as the engines' images are.
    std::vector<uint32_t> families;
    // VK_KHR_external_memory_fd + VK_EXT_external_memory_dma_buf (our ICD exports NT handles through them), and
    // VK_EXT_image_drm_format_modifier.
    bool external_memory = false;
    bool drm_modifier = false;

    struct Fns {
#define VGD_VK_FN(name) PFN_##name name = nullptr;
#include "vulkan_fns.inc"
#undef VGD_VK_FN
    } vk;

    // Loads the functions for the engine's device and finds what it supports for sharing.
    HRESULT init(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkPhysicalDevice physical, VkDevice device);
};

} // namespace vgd::umd
