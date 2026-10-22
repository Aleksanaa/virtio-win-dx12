// SPDX-License-Identifier: MIT
// Images on memory shared with another process through dma-buf: swapchain buffers and other surfaces the
// compositor reads, and surfaces other processes share with us. The engines cannot place an image on such memory,
// so we create the VkImage on the engine's device ourselves and lend it to one of the engine's resources.
//
// The layout contract between the two sides: a linear image of the same size and format at offset 0 of the memory,
// whose layout each side gets from its own plain linear image. Memory that is exported or imported must back an
// image declared external, and RADV offers dma-buf images only with DRM format modifiers; so with
// VK_EXT_image_drm_format_modifier the image is a LINEAR modifier image whose row pitch is given explicitly as that
// of the plain linear image. Without it, it is a plain linear image (which drivers accept but validation flags).
#pragma once

#include "vulkan_device.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace vgd::umd {

struct LinearImageDesc {
    VkFormat format = VK_FORMAT_UNDEFINED;
    // Further formats views of it may use (the image is then mutable).
    std::vector<VkFormat> view_formats;
    VkImageUsageFlags usage = 0;
    uint32_t width = 0, height = 0;
};

class LinearImage {
public:
    ~LinearImage();

    // A single-mip 2D image of `desc` on newly exported memory. *fd is the export: our ICD build passes the NT handle
    // of the allocation in place of an fd (kmt.h); the caller owns it.
    static HRESULT create_exported(const VulkanDevice& device, const LinearImageDesc& desc,
                                   std::unique_ptr<LinearImage>* out, int* fd);
    // The reverse: an image of `desc` on `size` bytes another process exported. On success the fd belongs to the
    // image; on failure it is still the caller's unless *consumed.
    static HRESULT create_imported(const VulkanDevice& device, const LinearImageDesc& desc, int fd, uint64_t size,
                                   std::unique_ptr<LinearImage>* out, bool* consumed);

    VkImage image() const { return image_; }
    uint64_t size() const { return size_; }
    uint32_t row_pitch() const { return row_pitch_; }

private:
    struct Plan;
    static HRESULT plan(const VulkanDevice& device, const LinearImageDesc& desc, bool import, Plan* out);
    HRESULT bind(bool declared_external, uint64_t linear_pitch);

    const VulkanDevice* device_ = nullptr;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    uint64_t size_ = 0;
    uint32_t row_pitch_ = 0;
};

} // namespace vgd::umd
