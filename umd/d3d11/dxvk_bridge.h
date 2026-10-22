// SPDX-License-Identifier: MIT
// What vgd3d11 needs from DXVK beyond its D3D11 API objects: creating its D3D11 device on the Venus ICD, the Vulkan
// handles behind it, wrapping our images as textures, and its format tables. dxvk_bridge.cpp is the one source that
// includes DXVK's internal headers (they clash with the WDK's); this header only needs the D3D11 and Vulkan APIs.
#pragma once

#include <d3d11_4.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace vgd::umd::dxvk {

struct DeviceHandles {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = {};
};

// A DXVK D3D11 device on the Venus ICD's GPU (the first adapter that is not a CPU), of feature level `level`, or
// of the highest DXVK offers there if `level` is 0. Of `extensions` (device extensions DXVK has no use for), those
// the GPU supports are enabled too.
HRESULT create_device(PFN_vkGetInstanceProcAddr gipa, D3D_FEATURE_LEVEL level, const char* const* extensions,
                      size_t extension_count, ID3D11Device5** out, DeviceHandles* handles);

// A texture over `image`, an image we created on the device for `desc` (see image_info), which stays ours.
HRESULT texture_from_image(ID3D11Device5* device, const D3D11_TEXTURE2D_DESC1& desc, VkImage image,
                           ID3D11Texture2D** out);

// The Vulkan format, the further view formats and the usage DXVK gives an image of `desc` (a single-sample colour
// 2D texture), for images we create for it: DXVK's views and copies assume them.
HRESULT image_info(ID3D11Device5* device, const D3D11_TEXTURE2D_DESC1& desc, VkFormat* format,
                   std::vector<VkFormat>* view_formats, VkImageUsageFlags* usage);

// The DXBC checksum of a container (dxbc-spirv's): DXVK keys its shaders on it.
void dxbc_checksum(const void* dxbc, size_t size, uint32_t checksum[4]);

// Lets DXVK see the settings forwarded from the registry; called before DXVK logs or reads its configuration.
void init_settings();

} // namespace vgd::umd::dxvk
