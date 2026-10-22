// SPDX-License-Identifier: MIT
#include "vulkan_device.h"

#include <cstring>

namespace vgd::umd {

const char* const kSharingExtensions[3] = {
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
};

namespace {

bool has_extension(const std::vector<VkExtensionProperties>& exts, const char* name)
{
    for (const auto& e : exts)
        if (!std::strcmp(e.extensionName, name))
            return true;
    return false;
}

} // namespace

HRESULT VulkanDevice::init(PFN_vkGetInstanceProcAddr gipa, VkInstance inst, VkPhysicalDevice phys, VkDevice dev)
{
    instance = inst;
    physical = phys;
    device = dev;
#define VGD_VK_FN(fn) vk.fn = reinterpret_cast<PFN_##fn>(gipa(instance, #fn));
#include "vulkan_fns.inc"
#undef VGD_VK_FN
    if (!vk.vkGetDeviceProcAddr)
        return E_FAIL;
    // Device-level functions straight from the device, skipping the instance's trampolines.
#define VGD_VK_FN(fn)                                                                                       \
    if (auto p = vk.vkGetDeviceProcAddr(device, #fn))                                                      \
        vk.fn = reinterpret_cast<PFN_##fn>(p);
#include "vulkan_fns.inc"
#undef VGD_VK_FN

    uint32_t count = 0;
    vk.vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    families.clear();
    for (uint32_t i = 0; i < count; i++)
        families.push_back(i);
    vk.vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vk.vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, exts.data());
    external_memory = has_extension(exts, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) &&
                      has_extension(exts, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) && vk.vkGetMemoryFdKHR &&
                      vk.vkGetMemoryFdPropertiesKHR;
    drm_modifier = external_memory && has_extension(exts, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    return S_OK;
}

} // namespace vgd::umd
