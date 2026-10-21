// SPDX-License-Identifier: MIT
#pragma once

#include <vgd/d3d12.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

namespace vgd {

// The Vulkan format a DXGI format is stored as; VK_FORMAT_UNDEFINED if we have none.
// Typeless formats map to a member of their family (resources created from them get VK_IMAGE_CREATE_MUTABLE_FORMAT).
VkFormat vk_format(DXGI_FORMAT format);
bool is_typeless(DXGI_FORMAT format);

// Size of one texel block, in bytes and texels, of a format (or of one aspect of a depth/stencil format).
struct FormatBlock {
    uint32_t bytes, width, height;
};
FormatBlock format_block(VkFormat format, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);
VkImageAspectFlags format_aspects(VkFormat format);
bool is_integer(VkFormat format);

struct FormatFeatures {
    VkFormatFeatureFlags2 optimal = 0;   // optimally tiled images
    VkFormatFeatureFlags2 buffer = 0;    // texel and vertex buffers
    // Sample counts of a 2D color/depth attachment image of this format (VK_SAMPLE_COUNT_1_BIT if not an attachment).
    VkSampleCountFlags samples = 0;
};

} // namespace vgd
