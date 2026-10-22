// SPDX-License-Identifier: MIT
#include "linear_image.h"
#include "log.h"

#include <algorithm>

namespace vgd::umd {

struct LinearImage::Plan {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    std::vector<VkFormat> view_formats;
    VkImageFormatListCreateInfo list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    VkSubresourceLayout plane{};
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier{
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
    uint64_t linear_pitch = 0;       // of the plain linear image
    bool declared_external = false;  // a LINEAR modifier image; its layout is asked of memory plane 0
    bool dedicated = false;          // its memory must be its own
};

namespace {

uint32_t memory_type(const VulkanDevice& d, uint32_t bits)
{
    VkPhysicalDeviceMemoryProperties props;
    d.vk.vkGetPhysicalDeviceMemoryProperties(d.physical, &props);
    uint32_t fallback = UINT32_MAX;
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if (!(bits & (1u << i)))
            continue;
        if (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
            return i;
        if (fallback == UINT32_MAX)
            fallback = i;
    }
    return fallback;
}

} // namespace

LinearImage::~LinearImage()
{
    if (image_)
        device_->vk.vkDestroyImage(device_->device, image_, nullptr);
    if (memory_)
        device_->vk.vkFreeMemory(device_->device, memory_, nullptr);
}

HRESULT LinearImage::plan(const VulkanDevice& d, const LinearImageDesc& desc, bool import, Plan* p)
{
    const char* what = import ? "imported image" : "shared image";
    if (!d.external_memory)
        return E_NOTIMPL;
    if (desc.format == VK_FORMAT_UNDEFINED || !desc.width || !desc.height)
        return E_INVALIDARG;
    VkImageCreateInfo& ci = p->ci;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = desc.format;
    ci.extent = {desc.width, desc.height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    // Other processes read it by row pitch; a shared image cannot use device-private tiling.
    ci.tiling = VK_IMAGE_TILING_LINEAR;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ci.usage = desc.usage;
    // The engines have no queue-family ownership to transfer: they use the image from any of their queues.
    if (d.families.size() > 1) {
        ci.sharingMode = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = static_cast<uint32_t>(d.families.size());
        ci.pQueueFamilyIndices = d.families.data();
    }
    p->view_formats.push_back(ci.format);
    for (VkFormat f : desc.view_formats)
        if (f != VK_FORMAT_UNDEFINED && std::find(p->view_formats.begin(), p->view_formats.end(), f) == p->view_formats.end())
            p->view_formats.push_back(f);
    if (p->view_formats.size() > 1) {
        ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        p->list.viewFormatCount = static_cast<uint32_t>(p->view_formats.size());
        p->list.pViewFormats = p->view_formats.data();
        ci.pNext = &p->list;
    }

    VkImageFormatProperties2 props{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    VkPhysicalDeviceImageFormatInfo2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, ci.pNext};
    query.format = ci.format;
    query.type = ci.imageType;
    query.tiling = ci.tiling;
    query.usage = ci.usage;
    query.flags = ci.flags;
    VkResult r = d.vk.vkGetPhysicalDeviceImageFormatProperties2(d.physical, &query, &props);
    if (r != VK_SUCCESS) {
        VGD_WARN("%s: format %d, usage 0x%x, flags 0x%x has no linear image (%d)", what, ci.format, ci.usage, ci.flags,
                 r);
        return E_NOTIMPL;
    }
    {
        VkImage probe = VK_NULL_HANDLE;
        r = d.vk.vkCreateImage(d.device, &ci, nullptr, &probe);
        if (r != VK_SUCCESS) {
            VGD_WARN("%s: vkCreateImage failed: %d", what, r);
            return E_OUTOFMEMORY;
        }
        const VkImageSubresource sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        VkSubresourceLayout linear{};
        d.vk.vkGetImageSubresourceLayout(d.device, probe, &sub, &linear);
        d.vk.vkDestroyImage(d.device, probe, nullptr);
        p->linear_pitch = linear.rowPitch;
    }

    constexpr uint64_t kDrmFormatModLinear = 0;
    p->external.pNext = ci.pNext;
    p->external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    p->plane.rowPitch = p->linear_pitch;
    p->modifier.pNext = &p->external;
    p->modifier.drmFormatModifier = kDrmFormatModLinear;
    p->modifier.drmFormatModifierPlaneCount = 1;
    p->modifier.pPlaneLayouts = &p->plane;
    if (d.drm_modifier) {
        VkPhysicalDeviceExternalImageFormatInfo ext_query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
                                                          ci.pNext};
        ext_query.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod_query{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT, &ext_query};
        mod_query.drmFormatModifier = kDrmFormatModLinear;
        mod_query.sharingMode = ci.sharingMode;
        mod_query.queueFamilyIndexCount = ci.queueFamilyIndexCount;
        mod_query.pQueueFamilyIndices = ci.pQueueFamilyIndices;
        query.pNext = &mod_query;
        query.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        VkExternalImageFormatProperties ext_props{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        props.pNext = &ext_props;
        r = d.vk.vkGetPhysicalDeviceImageFormatProperties2(d.physical, &query, &props);
        const VkExternalMemoryFeatureFlags features = ext_props.externalMemoryProperties.externalMemoryFeatures;
        const VkExternalMemoryFeatureFlags needed =
            import ? VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT : VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT;
        p->declared_external = r == VK_SUCCESS && (features & needed);
        p->dedicated = features & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT;
        if (!p->declared_external)
            VGD_TRACE("%s: format %d has no %s LINEAR modifier image (%d, 0x%x); using a plain linear image", what,
                      ci.format, import ? "importable" : "exportable", r, features);
    }
    if (p->declared_external) {
        ci.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        ci.pNext = &p->modifier;
    }
    return S_OK;
}

// After the memory is allocated: binds it and checks the layout both sides agree on.
HRESULT LinearImage::bind(bool declared_external, uint64_t linear_pitch)
{
    const VulkanDevice& d = *device_;
    VkResult r = d.vk.vkBindImageMemory(d.device, image_, memory_, 0);
    if (r != VK_SUCCESS)
        return E_OUTOFMEMORY;
    const VkImageSubresource sub{declared_external ? VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT : VK_IMAGE_ASPECT_COLOR_BIT,
                                 0, 0};
    VkSubresourceLayout layout{};
    d.vk.vkGetImageSubresourceLayout(d.device, image_, &sub, &layout);
    if (layout.offset != 0 || layout.rowPitch != linear_pitch || layout.rowPitch > UINT32_MAX) {
        VGD_WARN("shared image: unexpected layout (offset %llu, pitch %llu, linear pitch %llu)",
                 static_cast<unsigned long long>(layout.offset), static_cast<unsigned long long>(layout.rowPitch),
                 static_cast<unsigned long long>(linear_pitch));
        return E_NOTIMPL;
    }
    row_pitch_ = static_cast<uint32_t>(layout.rowPitch);
    return S_OK;
}

HRESULT LinearImage::create_exported(const VulkanDevice& d, const LinearImageDesc& desc,
                                     std::unique_ptr<LinearImage>* out, int* fd)
{
    *fd = -1;
    Plan p;
    HRESULT hr = plan(d, desc, false, &p);
    if (FAILED(hr))
        return hr;
    std::unique_ptr<LinearImage> s(new LinearImage);
    s->device_ = &d;
    if (d.vk.vkCreateImage(d.device, &p.ci, nullptr, &s->image_) != VK_SUCCESS)
        return E_OUTOFMEMORY;
    // Drivers may require external images (RADV: those with modifiers) to have memory of their own.
    VkMemoryDedicatedRequirements dedicated_req{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dedicated_req};
    const VkImageMemoryRequirementsInfo2 req_info{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, nullptr,
                                                  s->image_};
    d.vk.vkGetImageMemoryRequirements2(d.device, &req_info, &req);
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = s->image_;
    VkExportMemoryAllocateInfo export_info{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                                           p.dedicated || dedicated_req.requiresDedicatedAllocation ? &dedicated
                                                                                                   : nullptr};
    export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &export_info};
    mai.allocationSize = req.memoryRequirements.size;
    mai.memoryTypeIndex = memory_type(d, req.memoryRequirements.memoryTypeBits);
    if (mai.memoryTypeIndex == UINT32_MAX || d.vk.vkAllocateMemory(d.device, &mai, nullptr, &s->memory_) != VK_SUCCESS)
        return E_OUTOFMEMORY;
    s->size_ = req.memoryRequirements.size;
    hr = s->bind(p.declared_external, p.linear_pitch);
    if (FAILED(hr))
        return hr;

    VkMemoryGetFdInfoKHR get{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    get.memory = s->memory_;
    get.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkResult r = d.vk.vkGetMemoryFdKHR(d.device, &get, fd);
    if (r != VK_SUCCESS || *fd < 0) {
        VGD_WARN("shared image: vkGetMemoryFdKHR failed: %d", r);
        *fd = -1;
        return E_FAIL;
    }
    *out = std::move(s);
    return S_OK;
}

HRESULT LinearImage::create_imported(const VulkanDevice& d, const LinearImageDesc& desc, int fd, uint64_t size,
                                     std::unique_ptr<LinearImage>* out, bool* consumed)
{
    *consumed = false;
    Plan p;
    HRESULT hr = plan(d, desc, true, &p);
    if (FAILED(hr))
        return hr;
    std::unique_ptr<LinearImage> s(new LinearImage);
    s->device_ = &d;
    if (d.vk.vkCreateImage(d.device, &p.ci, nullptr, &s->image_) != VK_SUCCESS)
        return E_OUTOFMEMORY;
    VkMemoryDedicatedRequirements dedicated_req{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dedicated_req};
    const VkImageMemoryRequirementsInfo2 req_info{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, nullptr,
                                                  s->image_};
    d.vk.vkGetImageMemoryRequirements2(d.device, &req_info, &req);
    if (req.memoryRequirements.size > size) {
        VGD_WARN("imported image: needs %llu bytes, the memory has %llu (row pitch %llu)",
                 static_cast<unsigned long long>(req.memoryRequirements.size), static_cast<unsigned long long>(size),
                 static_cast<unsigned long long>(p.linear_pitch));
        return E_INVALIDARG;
    }
    VkMemoryFdPropertiesKHR fd_props{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    VkResult r = d.vk.vkGetMemoryFdPropertiesKHR(d.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd,
                                                 &fd_props);
    if (r != VK_SUCCESS) {
        VGD_WARN("imported image: vkGetMemoryFdPropertiesKHR failed: %d", r);
        return E_INVALIDARG;
    }
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = s->image_;
    VkImportMemoryFdInfoKHR import_info{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                                        p.dedicated || dedicated_req.requiresDedicatedAllocation ? &dedicated
                                                                                                : nullptr};
    import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import_info.fd = fd;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import_info};
    mai.allocationSize = size;
    mai.memoryTypeIndex = memory_type(d, req.memoryRequirements.memoryTypeBits & fd_props.memoryTypeBits);
    if (mai.memoryTypeIndex == UINT32_MAX) {
        VGD_WARN("imported image: no memory type in image bits 0x%x and fd bits 0x%x",
                 req.memoryRequirements.memoryTypeBits, fd_props.memoryTypeBits);
        return E_INVALIDARG;
    }
    r = d.vk.vkAllocateMemory(d.device, &mai, nullptr, &s->memory_);
    if (r != VK_SUCCESS) {
        VGD_WARN("imported image: vkAllocateMemory (import of %llu bytes) failed: %d",
                 static_cast<unsigned long long>(size), r);
        return E_INVALIDARG;
    }
    *consumed = true;
    s->size_ = size;
    hr = s->bind(p.declared_external, p.linear_pitch);
    if (FAILED(hr))
        return hr;
    *out = std::move(s);
    return S_OK;
}

} // namespace vgd::umd
