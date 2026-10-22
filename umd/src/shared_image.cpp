// SPDX-License-Identifier: MIT
#include "shared_image.h"
#include "log.h"
#include "vkd3d_bridge.h"

#include <algorithm>
#include <vector>

namespace vgd::umd {

struct SharedImage::Plan {
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

uint32_t memory_type(const Engine& e, uint32_t bits)
{
    VkPhysicalDeviceMemoryProperties props;
    e.vk.vkGetPhysicalDeviceMemoryProperties(e.physical, &props);
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

SharedImage::~SharedImage()
{
    // The resource only borrows the image: it goes first.
    resource_ = nullptr;
    if (image_)
        engine_->vk.vkDestroyImage(engine_->vk_device, image_, nullptr);
    if (memory_)
        engine_->vk.vkFreeMemory(engine_->vk_device, memory_, nullptr);
}

HRESULT SharedImage::plan(const std::shared_ptr<Engine>& engine, const D3D12_RESOURCE_DESC& desc,
                          const DXGI_FORMAT* castable, UINT castable_count, bool import, Plan* p)
{
    const char* what = import ? "imported texture" : "presentable texture";
    const Engine& e = *engine;
    if (!e.external_memory)
        return E_NOTIMPL;
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.MipLevels > 1 || desc.DepthOrArraySize > 1 ||
        desc.SampleDesc.Count > 1 || (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
        return E_NOTIMPL;
    VkImageCreateInfo& ci = p->ci;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = vgd_vkd3d_vk_format(desc.Format);
    if (ci.format == VK_FORMAT_UNDEFINED || !desc.Width || desc.Width > UINT32_MAX)
        return E_INVALIDARG;
    ci.extent = {static_cast<uint32_t>(desc.Width), std::max(desc.Height, 1u), 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    // Other processes read it by row pitch; a shared image cannot use device-private tiling.
    ci.tiling = VK_IMAGE_TILING_LINEAR;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!(desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
        ci.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
        ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
        ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    // D3D12 has no queue-family ownership: vkd3d-proton uses the image from any of its queues.
    if (e.families.size() > 1) {
        ci.sharingMode = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = static_cast<uint32_t>(e.families.size());
        ci.pQueueFamilyIndices = e.families.data();
    }
    p->view_formats.push_back(ci.format);
    for (UINT i = 0; i < castable_count; i++) {
        const VkFormat f = vgd_vkd3d_vk_format(castable[i]);
        if (f != VK_FORMAT_UNDEFINED && std::find(p->view_formats.begin(), p->view_formats.end(), f) == p->view_formats.end())
            p->view_formats.push_back(f);
    }
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
    VkResult r = e.vk.vkGetPhysicalDeviceImageFormatProperties2(e.physical, &query, &props);
    if (r != VK_SUCCESS) {
        VGD_WARN("%s: format %d, usage 0x%x, flags 0x%x has no linear image (%d)", what, ci.format, ci.usage, ci.flags,
                 r);
        return E_NOTIMPL;
    }
    {
        VkImage probe = VK_NULL_HANDLE;
        r = e.vk.vkCreateImage(e.vk_device, &ci, nullptr, &probe);
        if (r != VK_SUCCESS) {
            VGD_WARN("%s: vkCreateImage failed: %d", what, r);
            return E_OUTOFMEMORY;
        }
        const VkImageSubresource sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        VkSubresourceLayout linear{};
        e.vk.vkGetImageSubresourceLayout(e.vk_device, probe, &sub, &linear);
        e.vk.vkDestroyImage(e.vk_device, probe, nullptr);
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
    if (e.drm_modifier) {
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
        r = e.vk.vkGetPhysicalDeviceImageFormatProperties2(e.physical, &query, &props);
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

// After the memory is bound: the layout both sides agree on, the resource lent to vkd3d-proton, and the image in
// the layout vkd3d-proton keeps it in.
HRESULT SharedImage::finish(const D3D12_RESOURCE_DESC& desc, bool declared_external, uint64_t linear_pitch)
{
    Engine& e = *engine_;
    VkResult r = e.vk.vkBindImageMemory(e.vk_device, image_, memory_, 0);
    if (r != VK_SUCCESS)
        return E_OUTOFMEMORY;
    const VkImageSubresource sub{declared_external ? VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT : VK_IMAGE_ASPECT_COLOR_BIT,
                                 0, 0};
    VkSubresourceLayout layout{};
    e.vk.vkGetImageSubresourceLayout(e.vk_device, image_, &sub, &layout);
    if (layout.offset != 0 || layout.rowPitch != linear_pitch || layout.rowPitch > UINT32_MAX) {
        VGD_WARN("shared image: unexpected layout (offset %llu, pitch %llu, linear pitch %llu)",
                 static_cast<unsigned long long>(layout.offset), static_cast<unsigned long long>(layout.rowPitch),
                 static_cast<unsigned long long>(linear_pitch));
        return E_NOTIMPL;
    }
    row_pitch_ = static_cast<uint32_t>(layout.rowPitch);

    D3D12_RESOURCE_DESC1 desc1{};
    desc1.Dimension = desc.Dimension;
    desc1.Width = desc.Width;
    desc1.Height = desc.Height;
    desc1.DepthOrArraySize = 1;
    desc1.MipLevels = 1;
    desc1.Format = desc.Format;
    desc1.SampleDesc = {1, 0};
    desc1.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc1.Flags = desc.Flags;
    HRESULT hr = vgd_vkd3d_borrow_image(e.device.get(), &desc1, image_, resource_.put_void());
    if (FAILED(hr)) {
        VGD_WARN("shared image: vkd3d-proton did not take the image: 0x%08lx", hr);
        return hr;
    }
    VkImageLayout target = VK_IMAGE_LAYOUT_GENERAL;
    vgd_vkd3d_image_layout(e.device.get(), resource_.get(), D3D12_RESOURCE_STATE_COMMON, &target);
    // An imported image keeps what the exporter wrote only in practice: external images must start UNDEFINED, and
    // the spec's way to keep contents (an acquire from VK_QUEUE_FAMILY_EXTERNAL in the layout the exporter
    // released) needs a release yttrium's drivers do not make. Drivers leave linear memory alone in this transition.
    return e.run([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = target;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image_;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        e.vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                  nullptr, 0, nullptr, 1, &b);
    });
}

HRESULT SharedImage::create_exported(const std::shared_ptr<Engine>& engine, const D3D12_RESOURCE_DESC& desc,
                                     const DXGI_FORMAT* castable, UINT castable_count,
                                     std::unique_ptr<SharedImage>* out, int* fd)
{
    *fd = -1;
    Plan p;
    HRESULT hr = plan(engine, desc, castable, castable_count, false, &p);
    if (FAILED(hr))
        return hr;
    Engine& e = *engine;
    std::unique_ptr<SharedImage> s(new SharedImage);
    s->engine_ = engine;
    if (e.vk.vkCreateImage(e.vk_device, &p.ci, nullptr, &s->image_) != VK_SUCCESS)
        return E_OUTOFMEMORY;
    // Drivers may require external images (RADV: those with modifiers) to have memory of their own.
    VkMemoryDedicatedRequirements dedicated_req{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dedicated_req};
    const VkImageMemoryRequirementsInfo2 req_info{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, nullptr,
                                                  s->image_};
    e.vk.vkGetImageMemoryRequirements2(e.vk_device, &req_info, &req);
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = s->image_;
    VkExportMemoryAllocateInfo export_info{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                                           p.dedicated || dedicated_req.requiresDedicatedAllocation ? &dedicated
                                                                                                   : nullptr};
    export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &export_info};
    mai.allocationSize = req.memoryRequirements.size;
    mai.memoryTypeIndex = memory_type(e, req.memoryRequirements.memoryTypeBits);
    if (mai.memoryTypeIndex == UINT32_MAX || e.vk.vkAllocateMemory(e.vk_device, &mai, nullptr, &s->memory_) != VK_SUCCESS)
        return E_OUTOFMEMORY;
    s->size_ = req.memoryRequirements.size;
    hr = s->finish(desc, p.declared_external, p.linear_pitch);
    if (FAILED(hr))
        return hr;

    VkMemoryGetFdInfoKHR get{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    get.memory = s->memory_;
    get.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkResult r = e.vk.vkGetMemoryFdKHR(e.vk_device, &get, fd);
    if (r != VK_SUCCESS || *fd < 0) {
        VGD_WARN("presentable texture: vkGetMemoryFdKHR failed: %d", r);
        *fd = -1;
        return E_FAIL;
    }
    *out = std::move(s);
    return S_OK;
}

HRESULT SharedImage::create_imported(const std::shared_ptr<Engine>& engine, const D3D12_RESOURCE_DESC& desc,
                                     const DXGI_FORMAT* castable, UINT castable_count, int fd, uint64_t size,
                                     std::unique_ptr<SharedImage>* out, bool* consumed)
{
    *consumed = false;
    Plan p;
    HRESULT hr = plan(engine, desc, castable, castable_count, true, &p);
    if (FAILED(hr))
        return hr;
    Engine& e = *engine;
    std::unique_ptr<SharedImage> s(new SharedImage);
    s->engine_ = engine;
    if (e.vk.vkCreateImage(e.vk_device, &p.ci, nullptr, &s->image_) != VK_SUCCESS)
        return E_OUTOFMEMORY;
    VkMemoryDedicatedRequirements dedicated_req{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dedicated_req};
    const VkImageMemoryRequirementsInfo2 req_info{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, nullptr,
                                                  s->image_};
    e.vk.vkGetImageMemoryRequirements2(e.vk_device, &req_info, &req);
    if (req.memoryRequirements.size > size) {
        VGD_WARN("imported texture: needs %llu bytes, the memory has %llu (row pitch %llu)",
                 static_cast<unsigned long long>(req.memoryRequirements.size), static_cast<unsigned long long>(size),
                 static_cast<unsigned long long>(p.linear_pitch));
        return E_INVALIDARG;
    }
    VkMemoryFdPropertiesKHR fd_props{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    VkResult r = e.vk.vkGetMemoryFdPropertiesKHR(e.vk_device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd,
                                                 &fd_props);
    if (r != VK_SUCCESS) {
        VGD_WARN("imported texture: vkGetMemoryFdPropertiesKHR failed: %d", r);
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
    mai.memoryTypeIndex = memory_type(e, req.memoryRequirements.memoryTypeBits & fd_props.memoryTypeBits);
    if (mai.memoryTypeIndex == UINT32_MAX) {
        VGD_WARN("imported texture: no memory type in image bits 0x%x and fd bits 0x%x",
                 req.memoryRequirements.memoryTypeBits, fd_props.memoryTypeBits);
        return E_INVALIDARG;
    }
    r = e.vk.vkAllocateMemory(e.vk_device, &mai, nullptr, &s->memory_);
    if (r != VK_SUCCESS) {
        VGD_WARN("imported texture: vkAllocateMemory (import of %llu bytes) failed: %d",
                 static_cast<unsigned long long>(size), r);
        return E_INVALIDARG;
    }
    *consumed = true;
    s->size_ = size;
    hr = s->finish(desc, p.declared_external, p.linear_pitch);
    if (FAILED(hr))
        return hr;
    *out = std::move(s);
    return S_OK;
}

} // namespace vgd::umd
