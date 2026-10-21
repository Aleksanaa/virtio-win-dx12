// SPDX-License-Identifier: MIT
// Committed buffers and textures, image views, and the GPU-address map.
#include "internal.h"

#include <algorithm>
#include <bit>

namespace vgd {

namespace {

constexpr uint64_t kPlacementAlignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;   // 64 KiB

uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

// D3D12 resources carry no usage beyond their flags, so every usage they could be bound with is declared.
VkBufferUsageFlags buffer_usage(D3D12_RESOURCE_FLAGS flags)
{
    VkBufferUsageFlags u = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
        u |= VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    return u;
}

uint32_t full_mip_chain(const D3D12_RESOURCE_DESC& d)
{
    uint64_t m = std::max<uint64_t>(d.Width, d.Height);
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
        m = std::max<uint64_t>(m, d.DepthOrArraySize);
    return static_cast<uint32_t>(std::bit_width(std::max<uint64_t>(m, 1)));
}

// The image a texture becomes. `desc` must have its mip count filled in.
struct ImageInfo {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
};

HRESULT image_info(const Device::Impl& d, const D3D12_RESOURCE_DESC& desc, ImageInfo* out)
{
    VkImageCreateInfo& ci = out->ci;
    ci.format = d.resolve_format(desc.Format);
    if (ci.format == VK_FORMAT_UNDEFINED || desc.Width == 0 || desc.Width > UINT32_MAX)
        return E_INVALIDARG;
    ci.extent = {static_cast<uint32_t>(desc.Width), std::max(desc.Height, 1u), 1};
    ci.mipLevels = desc.MipLevels;
    ci.arrayLayers = std::max<uint32_t>(desc.DepthOrArraySize, 1);
    switch (desc.Dimension) {
    case D3D12_RESOURCE_DIMENSION_TEXTURE1D: ci.imageType = VK_IMAGE_TYPE_1D; break;
    case D3D12_RESOURCE_DIMENSION_TEXTURE2D: ci.imageType = VK_IMAGE_TYPE_2D; break;
    case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
        ci.imageType = VK_IMAGE_TYPE_3D;
        ci.extent.depth = ci.arrayLayers;
        ci.arrayLayers = 1;
        break;
    default: return E_INVALIDARG;
    }
    ci.samples = static_cast<VkSampleCountFlagBits>(std::max(desc.SampleDesc.Count, 1u));
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!(desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
        ci.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
        ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
        ci.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
        ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    // Typeless resources are viewed as any member of their family. EXTENDED_USAGE lets e.g. an sRGB view exist on
    // an image whose storage usage only the UNORM member supports.
    if (is_typeless(desc.Format))
        ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    if (ci.imageType == VK_IMAGE_TYPE_2D && ci.arrayLayers >= 6 && ci.extent.width == ci.extent.height &&
        ci.samples == VK_SAMPLE_COUNT_1_BIT)
        ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (ci.imageType == VK_IMAGE_TYPE_3D && (ci.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
        ci.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;   // RTVs of depth slices
    // D3D12 has no queue-family ownership, so resources are shared concurrently across our families.
    if (d.families.size() > 1) {
        ci.sharingMode = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = static_cast<uint32_t>(d.families.size());
        ci.pQueueFamilyIndices = d.families.data();
    }
    return S_OK;
}

D3D12_RESOURCE_DESC normalized(const D3D12_RESOURCE_DESC& in)
{
    D3D12_RESOURCE_DESC d = in;
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        if (d.MipLevels == 0)
            d.MipLevels = static_cast<UINT16>(full_mip_chain(d));
        d.Height = std::max(d.Height, 1u);
        d.DepthOrArraySize = std::max<UINT16>(d.DepthOrArraySize, 1);
        d.SampleDesc.Count = std::max(d.SampleDesc.Count, 1u);
    }
    return d;
}

} // namespace

// --- Address map -------------------------------------------------------------------------------------------

void Device::Impl::register_buffer(Resource::Impl* r)
{
    std::unique_lock g(va_lock);
    buffers_by_va.emplace(r->address, r);
    largest_buffer = std::max(largest_buffer, r->size);
}

void Device::Impl::unregister_buffer(Resource::Impl* r)
{
    std::unique_lock g(va_lock);
    auto [first, last] = buffers_by_va.equal_range(r->address);
    for (auto it = first; it != last; ++it)
        if (it->second == r) {
            buffers_by_va.erase(it);
            return;
        }
}

VkBuffer Device::Impl::find_buffer(VkDeviceAddress va, VkDeviceSize* offset, VkDeviceSize* size_left)
{
    std::shared_lock g(va_lock);
    // Back from the last buffer starting at or below `va`: an earlier, larger one may contain it when placed
    // buffers overlap. None starting more than the largest buffer's size below `va` can.
    for (auto it = buffers_by_va.upper_bound(va); it != buffers_by_va.begin();) {
        --it;
        const Resource::Impl* r = it->second;
        if (va - r->address >= largest_buffer)
            break;
        if (va - r->address < r->size) {
            *offset = va - r->address;
            *size_left = r->size - *offset;
            return r->buffer;
        }
    }
    return VK_NULL_HANDLE;
}

// --- Image initialization ----------------------------------------------------------------------------------

HRESULT Device::Impl::create_init_objects()
{
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = direct.family;
    VkResult r = vk.vkCreateCommandPool(device, &pci, nullptr, &init_pool);
    if (r != VK_SUCCESS)
        return hresult_from_vk(r);
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
    return hresult_from_vk(vk.vkCreateSemaphore(device, &sci, nullptr, &init_timeline));
}

HRESULT Device::Impl::init_submit(const std::function<void(VkCommandBuffer)>& record)
{
    std::lock_guard g(init_lock);
    uint64_t completed = 0;
    vk.vkGetSemaphoreCounterValue(device, init_timeline, &completed);
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    for (auto& [c, value] : init_cmds)
        if (value <= completed) {
            cmd = c;
            break;
        }
    if (!cmd) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = init_pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkResult r = vk.vkAllocateCommandBuffers(device, &ai, &cmd);
        if (r != VK_SUCCESS)
            return hresult_from_vk(r);
        init_cmds.push_back({cmd, 0});
    }
    vk.vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk.vkBeginCommandBuffer(cmd, &bi);
    record(cmd);
    VkResult r = vk.vkEndCommandBuffer(cmd);
    if (r != VK_SUCCESS)
        return hresult_from_vk(r);

    const uint64_t value = init_value.load() + 1;
    VkCommandBufferSubmitInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    ci.commandBuffer = cmd;
    VkSemaphoreSubmitInfo s{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    s.semaphore = init_timeline;
    s.value = value;
    s.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &ci;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &s;
    {
        std::lock_guard q(direct.submit_lock);
        r = vk.vkQueueSubmit2(direct.queue, 1, &si, VK_NULL_HANDLE);
    }
    if (r != VK_SUCCESS)
        return hresult_from_vk(r);
    for (auto& [c, v] : init_cmds)
        if (c == cmd)
            v = value;
    init_value.store(value);
    return S_OK;
}

HRESULT Device::Impl::init_image(VkImage image, VkImageAspectFlags aspects)
{
    return init_submit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {aspects, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vk.vkCmdPipelineBarrier2(cmd, &dep);
    });
}

// --- Resources ---------------------------------------------------------------------------------------------

Resource::Impl::~Impl()
{
    auto& vk = device->vk;
    for (auto& [key, v] : views)
        vk.vkDestroyImageView(device->device, v, nullptr);
    for (auto& [key, v] : buffer_views)
        vk.vkDestroyBufferView(device->device, v, nullptr);
    if (mapped && owns_memory)
        vk.vkUnmapMemory(device->device, memory);
    if (buffer) {
        device->unregister_buffer(this);
        vk.vkDestroyBuffer(device->device, buffer, nullptr);
    }
    if (image)
        vk.vkDestroyImage(device->device, image, nullptr);
    if (memory && owns_memory)
        vk.vkFreeMemory(device->device, memory, nullptr);
}

VkImageView Resource::Impl::view(const ImageViewKey& key)
{
    std::lock_guard g(views_lock);
    for (auto& [k, v] : views)
        if (k == key)
            return v;
    VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ci.image = image;
    ci.viewType = key.type;
    ci.format = key.format;
    ci.subresourceRange = {key.aspects, key.mip, key.mips, key.layer, key.layers};
    if (key.swizzle)
        ci.components = {VkComponentSwizzle(key.swizzle & 0xff), VkComponentSwizzle(key.swizzle >> 8 & 0xff),
                         VkComponentSwizzle(key.swizzle >> 16 & 0xff), VkComponentSwizzle(key.swizzle >> 24)};
    // Only the usage this view is for: a view format may not support everything the image was created for.
    VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    usage.usage = key.usage;
    ci.pNext = &usage;
    VkImageView v = VK_NULL_HANDLE;
    VkResult r = device->vk.vkCreateImageView(device->device, &ci, nullptr, &v);
    if (r != VK_SUCCESS) {
        log(LogLevel::warn, "vkCreateImageView (format %d, type %d) failed: %d", key.format, key.type, r);
        return VK_NULL_HANDLE;
    }
    views.push_back({key, v});
    return v;
}

Resource::Resource(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Resource::~Resource() = default;
const D3D12_RESOURCE_DESC& Resource::desc() const { return impl_->desc; }
uint64_t Resource::size() const { return impl_->size; }
D3D12_GPU_VIRTUAL_ADDRESS Resource::gpu_virtual_address() const { return impl_->address; }
void* Resource::cpu_pointer() const { return impl_->mapped; }

HRESULT Device::create_committed_buffer(D3D12_HEAP_TYPE heap_type, uint64_t size, D3D12_RESOURCE_FLAGS flags,
                                        std::unique_ptr<Resource>* out)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc = {1, 0};
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return create_committed_resource(heap_type, desc, out);
}

namespace {

// The Vulkan object a resource becomes, without memory: res->buffer or res->image, with what it needs of memory.
HRESULT create_unbound(Device::Impl& d, Resource::Impl& res, VkMemoryRequirements* req)
{
    const D3D12_RESOURCE_DESC& desc = res.desc;
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        res.size = desc.Width;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = desc.Width;
        bci.usage = buffer_usage(desc.Flags);
        if (d.families.size() > 1) {
            bci.sharingMode = VK_SHARING_MODE_CONCURRENT;
            bci.queueFamilyIndexCount = static_cast<uint32_t>(d.families.size());
            bci.pQueueFamilyIndices = d.families.data();
        }
        VkResult r = d.vk.vkCreateBuffer(d.device, &bci, nullptr, &res.buffer);
        if (r != VK_SUCCESS)
            return hresult_from_vk(r);
        d.vk.vkGetBufferMemoryRequirements(d.device, res.buffer, req);
        return S_OK;
    }
    ImageInfo info;
    HRESULT hr = image_info(d, desc, &info);
    if (FAILED(hr))
        return hr;
    res.format = info.ci.format;
    res.aspects = format_aspects(info.ci.format);
    VkResult r = d.vk.vkCreateImage(d.device, &info.ci, nullptr, &res.image);
    if (r != VK_SUCCESS) {
        log(LogLevel::warn, "vkCreateImage (format %d, %ux%ux%u, usage 0x%x) failed: %d", info.ci.format,
            info.ci.extent.width, info.ci.extent.height, info.ci.arrayLayers, info.ci.usage, r);
        return hresult_from_vk(r);
    }
    d.vk.vkGetImageMemoryRequirements(d.device, res.image, req);
    res.size = req->size;
    return S_OK;
}

// Binds the object at `offset` of res.memory; `mapped` is the CPU address of memory offset 0, if mapped.
HRESULT bind(Device::Impl& d, Resource::Impl& res, uint64_t offset, void* mapped)
{
    if (res.buffer) {
        VkResult r = d.vk.vkBindBufferMemory(d.device, res.buffer, res.memory, offset);
        if (r != VK_SUCCESS)
            return hresult_from_vk(r);
        VkBufferDeviceAddressInfo bda{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        bda.buffer = res.buffer;
        res.address = d.vk.vkGetBufferDeviceAddress(d.device, &bda);
        d.register_buffer(&res);
        if (mapped)
            res.mapped = static_cast<char*>(mapped) + offset;
        return S_OK;
    }
    VkResult r = d.vk.vkBindImageMemory(d.device, res.image, res.memory, offset);
    if (r != VK_SUCCESS)
        return hresult_from_vk(r);
    return d.init_image(res.image, res.aspects);
}

} // namespace

HRESULT Device::create_committed_resource(D3D12_HEAP_TYPE heap_type, const D3D12_RESOURCE_DESC& in_desc,
                                          std::unique_ptr<Resource>* out)
{
    if (!out)
        return E_INVALIDARG;
    if (heap_type != D3D12_HEAP_TYPE_DEFAULT && heap_type != D3D12_HEAP_TYPE_UPLOAD &&
        heap_type != D3D12_HEAP_TYPE_READBACK)
        return E_NOTIMPL;
    const D3D12_RESOURCE_DESC desc = normalized(in_desc);
    const bool is_buffer = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
    if (is_buffer ? desc.Width == 0 : heap_type != D3D12_HEAP_TYPE_DEFAULT)
        return E_INVALIDARG;   // textures in CPU-visible heaps need custom heaps with row-major layouts

    Impl& d = *impl_;
    auto res = std::make_unique<Resource::Impl>();
    res->device = &d;
    res->desc = desc;
    VkMemoryRequirements req;
    HRESULT hr = create_unbound(d, *res, &req);
    if (FAILED(hr))
        return hr;
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    VkMemoryAllocateFlagsInfo flags_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    if (is_buffer) {
        flags_info.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        mai.pNext = &flags_info;
    } else {
        dedicated.image = res->image;
        mai.pNext = &dedicated;
    }
    const uint32_t type = d.find_memory_type(req.memoryTypeBits, heap_type);
    if (type == UINT32_MAX)
        return E_OUTOFMEMORY;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    VkResult r = d.vk.vkAllocateMemory(d.device, &mai, nullptr, &res->memory);
    if (r != VK_SUCCESS)
        return hresult_from_vk(r);
    void* mapped = nullptr;
    if (heap_type != D3D12_HEAP_TYPE_DEFAULT) {
        r = d.vk.vkMapMemory(d.device, res->memory, 0, VK_WHOLE_SIZE, 0, &mapped);
        if (r != VK_SUCCESS)
            return hresult_from_vk(r);
        res->mapped = mapped;   // unmapped by the destructor, whatever bind() does
    }
    hr = bind(d, *res, 0, mapped);
    if (FAILED(hr))
        return hr;
    out->reset(new Resource(std::move(res)));
    return S_OK;
}

// --- Heaps and placed resources ----------------------------------------------------------------------------

Heap::Impl::~Impl()
{
    if (mapped)
        device->vk.vkUnmapMemory(device->device, memory);
    if (memory)
        device->vk.vkFreeMemory(device->device, memory, nullptr);
}

Heap::Heap(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Heap::~Heap() = default;
D3D12_HEAP_TYPE Heap::type() const { return impl_->type; }
uint64_t Heap::size() const { return impl_->size; }
void* Heap::cpu_pointer() const { return impl_->mapped; }

HRESULT Device::create_heap(D3D12_HEAP_TYPE heap_type, uint64_t size, D3D12_HEAP_FLAGS flags,
                            std::unique_ptr<Heap>* out)
{
    if (!out || size == 0)
        return E_INVALIDARG;
    if (heap_type != D3D12_HEAP_TYPE_DEFAULT && heap_type != D3D12_HEAP_TYPE_UPLOAD &&
        heap_type != D3D12_HEAP_TYPE_READBACK)
        return E_NOTIMPL;
    Impl& d = *impl_;

    // The memory types every allowed category can use. Vulkan makes memoryTypeBits a function of the buffer's
    // usage and flags, and of an image's tiling, flags and (for depth/stencil) format, never of sizes, so one
    // representative of each category stands for all of it (Vulkan spec, "Resource Memory Association").
    uint32_t type_bits = ~0u;
    if (!(flags & D3D12_HEAP_FLAG_DENY_BUFFERS)) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = 65536;
        bci.usage = buffer_usage(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        VkDeviceBufferMemoryRequirements q{VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS};
        q.pCreateInfo = &bci;
        VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        d.vk.vkGetDeviceBufferMemoryRequirements(d.device, &q, &req);
        type_bits &= req.memoryRequirements.memoryTypeBits;
    }
    auto image_bits = [&](DXGI_FORMAT format, D3D12_RESOURCE_FLAGS rflags) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = desc.Height = 64;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc = {1, 0};
        desc.Flags = rflags;
        ImageInfo info;
        if (FAILED(image_info(d, desc, &info)))
            return ~0u;
        VkDeviceImageMemoryRequirements q{VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS};
        q.pCreateInfo = &info.ci;
        VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        d.vk.vkGetDeviceImageMemoryRequirements(d.device, &q, &req);
        return req.memoryRequirements.memoryTypeBits;
    };
    if (!(flags & D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES))
        type_bits &= image_bits(DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
    if (!(flags & D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES)) {
        type_bits &= image_bits(DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        type_bits &= image_bits(DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        type_bits &= image_bits(DXGI_FORMAT_D32_FLOAT_S8X24_UINT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    }

    auto heap = std::make_unique<Heap::Impl>();
    heap->device = &d;
    heap->type = heap_type;
    heap->size = size;
    heap->memory_type = d.find_memory_type(type_bits, heap_type);
    if (heap->memory_type == UINT32_MAX) {
        log(LogLevel::warn, "heap: no memory type of heap type %d for every allowed category (bits 0x%x, flags 0x%x)",
            heap_type, type_bits, flags);
        return E_OUTOFMEMORY;
    }
    VkMemoryAllocateFlagsInfo flags_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags_info.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;   // for buffers placed in it
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags_info};
    mai.allocationSize = size;
    mai.memoryTypeIndex = heap->memory_type;
    VkResult r = d.vk.vkAllocateMemory(d.device, &mai, nullptr, &heap->memory);
    if (r != VK_SUCCESS)
        return hresult_from_vk(r);
    if (heap_type != D3D12_HEAP_TYPE_DEFAULT) {
        r = d.vk.vkMapMemory(d.device, heap->memory, 0, VK_WHOLE_SIZE, 0, &heap->mapped);
        if (r != VK_SUCCESS)
            return hresult_from_vk(r);
    }
    out->reset(new Heap(std::move(heap)));
    return S_OK;
}

HRESULT Device::create_placed_resource(Heap& heap, uint64_t offset, const D3D12_RESOURCE_DESC& in_desc,
                                       std::unique_ptr<Resource>* out)
{
    if (!out)
        return E_INVALIDARG;
    Impl& d = *impl_;
    Heap::Impl& h = heap.impl();
    const D3D12_RESOURCE_DESC desc = normalized(in_desc);
    const bool is_buffer = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
    if (is_buffer ? desc.Width == 0 : h.type != D3D12_HEAP_TYPE_DEFAULT)
        return E_INVALIDARG;   // textures in CPU-visible heaps need custom heaps with row-major layouts

    auto res = std::make_unique<Resource::Impl>();
    res->device = &d;
    res->desc = desc;
    res->owns_memory = false;
    res->memory = h.memory;
    VkMemoryRequirements req;
    HRESULT hr = create_unbound(d, *res, &req);
    if (FAILED(hr))
        return hr;
    if (!(req.memoryTypeBits & (1u << h.memory_type)) || offset % req.alignment || offset > h.size ||
        req.size > h.size - offset) {
        log(LogLevel::warn, "placed resource: does not fit heap (offset %llu, size %llu, alignment %llu, types 0x%x; "
            "heap %llu bytes, type %u)", static_cast<unsigned long long>(offset),
            static_cast<unsigned long long>(req.size), static_cast<unsigned long long>(req.alignment),
            req.memoryTypeBits, static_cast<unsigned long long>(h.size), h.memory_type);
        return E_INVALIDARG;
    }
    hr = bind(d, *res, offset, h.mapped);
    if (FAILED(hr))
        return hr;
    out->reset(new Resource(std::move(res)));
    return S_OK;
}

namespace {

// A linear image shared with another process through dma-buf memory, before its memory is bound.
//
// The layout contract with the other side (yttrium's D3D10 UMD in the compositor and its Vulkan WSI, importing ours
// or exporting its own): a linear image of the same size and format at offset 0 of the memory, whose layout each
// side gets from its own plain linear image (yttrium_venus2_resource.c, "Shared single-sample colour images use
// linear tiling"). Memory that is exported or imported must back an image declared external, and RADV offers
// dma-buf images only with DRM format modifiers; so with VK_EXT_image_drm_format_modifier ours is a LINEAR modifier
// image whose row pitch is given explicitly as that of the plain linear image. Without it, it is a plain linear
// image like yttrium's own shared images (which drivers accept but validation flags).
struct SharedImage {
    std::unique_ptr<Resource::Impl> res;
    VkMemoryRequirements req{};
    uint64_t row_pitch = 0;          // of the plain linear image
    bool declared_external = false;  // a LINEAR modifier image; its layout is asked of memory plane 0
    bool dedicated = false;          // its memory must be its own
    bool import = false;
};

HRESULT create_shared_image(Device::Impl& d, const D3D12_RESOURCE_DESC& desc, const DXGI_FORMAT* castable,
                            uint32_t castable_count, bool import, SharedImage* out)
{
    const char* what = import ? "imported texture" : "presentable texture";
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.MipLevels != 1 || desc.DepthOrArraySize != 1 ||
        desc.SampleDesc.Count != 1 || (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
        return E_NOTIMPL;
    ImageInfo info;
    HRESULT hr = image_info(d, desc, &info);
    if (FAILED(hr))
        return hr;
    // Other processes read it by row pitch; a shared image cannot use device-private tiling.
    info.ci.tiling = VK_IMAGE_TILING_LINEAR;
    std::vector<VkFormat> view_formats{info.ci.format};
    for (uint32_t i = 0; i < castable_count; i++) {
        const VkFormat f = d.resolve_format(castable[i]);
        if (f != VK_FORMAT_UNDEFINED && std::find(view_formats.begin(), view_formats.end(), f) == view_formats.end())
            view_formats.push_back(f);
    }
    VkImageFormatListCreateInfo list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    if (view_formats.size() > 1) {
        info.ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        list.viewFormatCount = static_cast<uint32_t>(view_formats.size());
        list.pViewFormats = view_formats.data();
    }
    info.ci.pNext = view_formats.size() > 1 ? &list : nullptr;

    VkImageFormatProperties2 props{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    VkPhysicalDeviceImageFormatInfo2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, info.ci.pNext};
    query.format = info.ci.format;
    query.type = info.ci.imageType;
    query.tiling = info.ci.tiling;
    query.usage = info.ci.usage;
    query.flags = info.ci.flags;
    VkResult r = d.ivk->vkGetPhysicalDeviceImageFormatProperties2(d.physical->handle, &query, &props);
    if (r != VK_SUCCESS) {
        log(LogLevel::warn, "%s: format %d, usage 0x%x, flags 0x%x has no linear image (%d)", what, info.ci.format,
            info.ci.usage, info.ci.flags, r);
        return E_NOTIMPL;
    }
    {
        VkImage probe = VK_NULL_HANDLE;
        r = d.vk.vkCreateImage(d.device, &info.ci, nullptr, &probe);
        if (r != VK_SUCCESS) {
            log(LogLevel::warn, "%s: vkCreateImage failed: %d", what, r);
            return hresult_from_vk(r);
        }
        const VkImageSubresource sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        VkSubresourceLayout linear{};
        d.vk.vkGetImageSubresourceLayout(d.device, probe, &sub, &linear);
        d.vk.vkDestroyImage(d.device, probe, nullptr);
        out->row_pitch = linear.rowPitch;
    }

    constexpr uint64_t kDrmFormatModLinear = 0;
    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, info.ci.pNext};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    const VkSubresourceLayout plane{0, 0, out->row_pitch, 0, 0};
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier{
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT, &external};
    modifier.drmFormatModifier = kDrmFormatModLinear;
    modifier.drmFormatModifierPlaneCount = 1;
    modifier.pPlaneLayouts = &plane;
    if (d.physical->desc.drm_format_modifier) {
        VkPhysicalDeviceExternalImageFormatInfo ext_query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
                                                          info.ci.pNext};
        ext_query.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod_query{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT, &ext_query};
        mod_query.drmFormatModifier = kDrmFormatModLinear;
        mod_query.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        query.pNext = &mod_query;
        query.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        VkExternalImageFormatProperties ext_props{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        props.pNext = &ext_props;
        r = d.ivk->vkGetPhysicalDeviceImageFormatProperties2(d.physical->handle, &query, &props);
        const VkExternalMemoryFeatureFlags features = ext_props.externalMemoryProperties.externalMemoryFeatures;
        const VkExternalMemoryFeatureFlags needed =
            import ? VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT : VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT;
        out->declared_external = r == VK_SUCCESS && (features & needed);
        out->dedicated = features & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT;
        if (!out->declared_external)
            log(LogLevel::info, "%s: format %d has no %s LINEAR modifier image (%d, 0x%x); using a plain linear image",
                what, info.ci.format, import ? "importable" : "exportable", r, features);
    }
    if (out->declared_external) {
        info.ci.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        info.ci.pNext = &modifier;
    }

    auto res = std::make_unique<Resource::Impl>();
    res->device = &d;
    res->desc = desc;
    res->format = info.ci.format;
    res->aspects = format_aspects(info.ci.format);
    r = d.vk.vkCreateImage(d.device, &info.ci, nullptr, &res->image);
    if (r != VK_SUCCESS) {
        log(LogLevel::warn, "%s: vkCreateImage failed: %d", what, r);
        return hresult_from_vk(r);
    }
    // Drivers may require external images (RADV: those with modifiers) to have memory of their own.
    VkMemoryDedicatedRequirements dedicated_req{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 req2{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dedicated_req};
    const VkImageMemoryRequirementsInfo2 req_info{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, nullptr,
                                                  res->image};
    d.vk.vkGetImageMemoryRequirements2(d.device, &req_info, &req2);
    out->req = req2.memoryRequirements;
    res->size = out->req.size;
    out->dedicated = out->dedicated || dedicated_req.requiresDedicatedAllocation;
    out->res = std::move(res);
    out->import = import;
    return S_OK;
}

// After the memory is bound: the layout both sides agree on, and the image in GENERAL.
HRESULT finish_shared_image(Device::Impl& d, SharedImage& s)
{
    const char* what = s.import ? "imported texture" : "presentable texture";
    Resource::Impl& res = *s.res;
    VkResult r = d.vk.vkBindImageMemory(d.device, res.image, res.memory, 0);
    if (r != VK_SUCCESS)
        return hresult_from_vk(r);
    const VkImageSubresource sub{s.declared_external ? VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT
                                                     : VK_IMAGE_ASPECT_COLOR_BIT,
                                 0, 0};
    VkSubresourceLayout layout{};
    d.vk.vkGetImageSubresourceLayout(d.device, res.image, &sub, &layout);
    if (layout.offset != 0 || layout.rowPitch != s.row_pitch || layout.rowPitch > UINT32_MAX) {
        log(LogLevel::warn, "%s: unexpected layout (offset %llu, pitch %llu, linear pitch %llu)", what,
            static_cast<unsigned long long>(layout.offset), static_cast<unsigned long long>(layout.rowPitch),
            static_cast<unsigned long long>(s.row_pitch));
        return E_NOTIMPL;
    }
    // An imported image keeps what the exporter wrote only in practice: external images must start UNDEFINED, and
    // the spec's way to keep contents (an acquire from VK_QUEUE_FAMILY_EXTERNAL in the layout the exporter
    // released) needs a release yttrium's drivers do not make. Drivers leave linear memory alone in this transition
    // (RADV, llvmpipe: core/tests/present.cpp reads an import back).
    return d.init_image(res.image, res.aspects);
}

} // namespace

HRESULT Device::create_presentable_texture(const D3D12_RESOURCE_DESC& in_desc, const DXGI_FORMAT* castable,
                                           uint32_t castable_count, std::unique_ptr<Resource>* out,
                                           ExportedTexture* exported)
{
    if (!out || !exported)
        return E_INVALIDARG;
    Impl& d = *impl_;
    if (!d.physical->desc.external_memory || !d.vk.vkGetMemoryFdKHR)
        return E_NOTIMPL;
    SharedImage s;
    HRESULT hr = create_shared_image(d, normalized(in_desc), castable, castable_count, false, &s);
    if (FAILED(hr))
        return hr;

    VkMemoryDedicatedAllocateInfo dedicated_info{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated_info.image = s.res->image;
    VkExportMemoryAllocateInfo export_info{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                                           s.dedicated ? &dedicated_info : nullptr};
    export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &export_info};
    mai.allocationSize = s.req.size;
    mai.memoryTypeIndex = d.find_memory_type(s.req.memoryTypeBits, D3D12_HEAP_TYPE_DEFAULT);
    if (mai.memoryTypeIndex == UINT32_MAX)
        return E_OUTOFMEMORY;
    VkResult r = d.vk.vkAllocateMemory(d.device, &mai, nullptr, &s.res->memory);
    if (r != VK_SUCCESS)
        return hresult_from_vk(r);
    hr = finish_shared_image(d, s);
    if (FAILED(hr))
        return hr;

    VkMemoryGetFdInfoKHR get{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    get.memory = s.res->memory;
    get.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    int fd = -1;
    r = d.vk.vkGetMemoryFdKHR(d.device, &get, &fd);
    if (r != VK_SUCCESS || fd < 0) {
        log(LogLevel::warn, "presentable texture: vkGetMemoryFdKHR failed: %d", r);
        return r != VK_SUCCESS ? hresult_from_vk(r) : E_FAIL;
    }
    exported->fd = fd;
    exported->size = s.req.size;
    exported->row_pitch = static_cast<uint32_t>(s.row_pitch);
    out->reset(new Resource(std::move(s.res)));
    return S_OK;
}

HRESULT Device::import_texture(const D3D12_RESOURCE_DESC& in_desc, const DXGI_FORMAT* castable,
                               uint32_t castable_count, int fd, uint64_t size, std::unique_ptr<Resource>* out,
                               bool* fd_consumed)
{
    if (!out || !fd_consumed || fd < 0)
        return E_INVALIDARG;
    *fd_consumed = false;
    Impl& d = *impl_;
    if (!d.physical->desc.external_memory || !d.vk.vkGetMemoryFdPropertiesKHR)
        return E_NOTIMPL;
    SharedImage s;
    HRESULT hr = create_shared_image(d, normalized(in_desc), castable, castable_count, true, &s);
    if (FAILED(hr))
        return hr;
    if (s.req.size > size) {
        log(LogLevel::warn, "imported texture: needs %llu bytes, the memory has %llu (row pitch %llu)",
            static_cast<unsigned long long>(s.req.size), static_cast<unsigned long long>(size),
            static_cast<unsigned long long>(s.row_pitch));
        return E_INVALIDARG;
    }

    VkMemoryFdPropertiesKHR fd_props{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    VkResult r = d.vk.vkGetMemoryFdPropertiesKHR(d.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd,
                                                 &fd_props);
    if (r != VK_SUCCESS) {
        log(LogLevel::warn, "imported texture: vkGetMemoryFdPropertiesKHR failed: %d", r);
        return hresult_from_vk(r);
    }
    VkMemoryDedicatedAllocateInfo dedicated_info{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated_info.image = s.res->image;
    VkImportMemoryFdInfoKHR import_info{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                                        s.dedicated ? &dedicated_info : nullptr};
    import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import_info.fd = fd;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import_info};
    mai.allocationSize = size;
    mai.memoryTypeIndex = d.find_memory_type(s.req.memoryTypeBits & fd_props.memoryTypeBits, D3D12_HEAP_TYPE_DEFAULT);
    if (mai.memoryTypeIndex == UINT32_MAX) {
        log(LogLevel::warn, "imported texture: no memory type in image bits 0x%x and fd bits 0x%x",
            s.req.memoryTypeBits, fd_props.memoryTypeBits);
        return E_INVALIDARG;
    }
    r = d.vk.vkAllocateMemory(d.device, &mai, nullptr, &s.res->memory);
    if (r != VK_SUCCESS) {
        log(LogLevel::warn, "imported texture: vkAllocateMemory (import of %llu bytes) failed: %d",
            static_cast<unsigned long long>(size), r);
        return hresult_from_vk(r);
    }
    *fd_consumed = true;
    hr = finish_shared_image(d, s);
    if (FAILED(hr))
        return hr;
    out->reset(new Resource(std::move(s.res)));
    return S_OK;
}

HRESULT Device::resource_allocation_info(const D3D12_RESOURCE_DESC& in_desc, uint64_t* size,
                                         uint64_t* alignment) const
{
    const D3D12_RESOURCE_DESC desc = normalized(in_desc);
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        *size = align_up(desc.Width, kPlacementAlignment);
        *alignment = kPlacementAlignment;
        return S_OK;
    }
    ImageInfo info;
    HRESULT hr = image_info(*impl_, desc, &info);
    if (FAILED(hr))
        return hr;
    VkDeviceImageMemoryRequirements dreq{VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS};
    dreq.pCreateInfo = &info.ci;
    VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    impl_->vk.vkGetDeviceImageMemoryRequirements(impl_->device, &dreq, &req);
    // D3D12 placement alignments are 64 KiB (4 MiB for MSAA); never ask for less.
    const uint64_t a = std::max<uint64_t>(req.memoryRequirements.alignment,
                                          desc.SampleDesc.Count > 1 ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT
                                                                    : kPlacementAlignment);
    *alignment = a;
    *size = align_up(req.memoryRequirements.size, a);
    return S_OK;
}

// --- Views -------------------------------------------------------------------------------------------------

HRESULT Device::create_render_target_view(Resource& resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc,
                                          RenderTargetView* out)
{
    Resource::Impl& r = resource.impl();
    if (!out || !r.image)
        return E_INVALIDARG;
    const auto& rd = r.desc;
    ImageViewKey key{VK_IMAGE_VIEW_TYPE_2D, r.format, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1, 0,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    if (desc && desc->Format != DXGI_FORMAT_UNKNOWN)
        key.format = impl_->resolve_format(desc->Format);
    auto layers = [&](UINT first, UINT size) {
        key.layer = first;
        key.layers = size == UINT_MAX ? r.array_layers() - first : size;
    };
    if (!desc) {
        key.layers = r.array_layers();
    } else {
        switch (desc->ViewDimension) {
        case D3D12_RTV_DIMENSION_TEXTURE1D: key.mip = desc->Texture1D.MipSlice; break;
        case D3D12_RTV_DIMENSION_TEXTURE1DARRAY:
            key.mip = desc->Texture1DArray.MipSlice;
            layers(desc->Texture1DArray.FirstArraySlice, desc->Texture1DArray.ArraySize);
            break;
        case D3D12_RTV_DIMENSION_TEXTURE2D:
            key.mip = desc->Texture2D.MipSlice;
            if (desc->Texture2D.PlaneSlice)
                return E_INVALIDARG;
            break;
        case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:
            key.mip = desc->Texture2DArray.MipSlice;
            layers(desc->Texture2DArray.FirstArraySlice, desc->Texture2DArray.ArraySize);
            break;
        case D3D12_RTV_DIMENSION_TEXTURE2DMS: break;
        case D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY:
            layers(desc->Texture2DMSArray.FirstArraySlice, desc->Texture2DMSArray.ArraySize);
            break;
        case D3D12_RTV_DIMENSION_TEXTURE3D:
            // Depth slices of a 3D image, through its 2D-array compatibility.
            key.mip = desc->Texture3D.MipSlice;
            layers(desc->Texture3D.FirstWSlice,
                   desc->Texture3D.WSize == UINT_MAX
                       ? std::max<uint32_t>(rd.DepthOrArraySize >> key.mip, 1) - desc->Texture3D.FirstWSlice
                       : desc->Texture3D.WSize);
            break;
        default:
            log(LogLevel::warn, "render target view dimension %d not supported", desc->ViewDimension);
            return E_INVALIDARG;
        }
    }
    if (key.format == VK_FORMAT_UNDEFINED || key.mip >= rd.MipLevels || key.layers == 0)
        return E_INVALIDARG;
    if (key.layers > 1)
        key.type = rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? VK_IMAGE_VIEW_TYPE_1D_ARRAY
                                                                       : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    else if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D)
        key.type = VK_IMAGE_VIEW_TYPE_1D;
    // A 3D image's RTV is a 2D (array) view, so the subresource range names layers, not depth.

    RenderTargetView v;
    v.resource = &resource;
    v.view = r.view(key);
    if (!v.view)
        return E_OUTOFMEMORY;
    v.format = key.format;
    v.width = std::max<uint32_t>(static_cast<uint32_t>(rd.Width >> key.mip), 1);
    v.height = std::max<uint32_t>(rd.Height >> key.mip, 1);
    v.mip = key.mip;
    v.first_layer = key.layer;
    v.layers = key.layers;
    *out = v;
    return S_OK;
}

} // namespace vgd

namespace vgd {

HRESULT Device::create_depth_stencil_view(Resource& resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
                                          DepthStencilView* out)
{
    Resource::Impl& r = resource.impl();
    if (!out || !r.image || !(r.aspects & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)))
        return E_INVALIDARG;
    const auto& rd = r.desc;
    // Depth/stencil images are viewed in their own format, with every plane they have.
    ImageViewKey key{VK_IMAGE_VIEW_TYPE_2D, r.format, r.aspects, 0, 1, 0, 1, 0,
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT};
    auto layers = [&](UINT first, UINT size) {
        key.layer = first;
        key.layers = size == UINT_MAX ? r.array_layers() - first : size;
    };
    if (!desc) {
        key.layers = r.array_layers();
    } else {
        switch (desc->ViewDimension) {
        case D3D12_DSV_DIMENSION_TEXTURE1D: key.mip = desc->Texture1D.MipSlice; break;
        case D3D12_DSV_DIMENSION_TEXTURE1DARRAY:
            key.mip = desc->Texture1DArray.MipSlice;
            layers(desc->Texture1DArray.FirstArraySlice, desc->Texture1DArray.ArraySize);
            break;
        case D3D12_DSV_DIMENSION_TEXTURE2D: key.mip = desc->Texture2D.MipSlice; break;
        case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
            key.mip = desc->Texture2DArray.MipSlice;
            layers(desc->Texture2DArray.FirstArraySlice, desc->Texture2DArray.ArraySize);
            break;
        case D3D12_DSV_DIMENSION_TEXTURE2DMS: break;
        case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY:
            layers(desc->Texture2DMSArray.FirstArraySlice, desc->Texture2DMSArray.ArraySize);
            break;
        default:
            log(LogLevel::warn, "depth/stencil view dimension %d not supported", desc->ViewDimension);
            return E_INVALIDARG;
        }
    }
    if (key.mip >= rd.MipLevels || key.layers == 0 || key.layer + key.layers > r.array_layers())
        return E_INVALIDARG;
    if (key.layers > 1)
        key.type = rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? VK_IMAGE_VIEW_TYPE_1D_ARRAY
                                                                       : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    else if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D)
        key.type = VK_IMAGE_VIEW_TYPE_1D;

    DepthStencilView v;
    v.resource = &resource;
    v.view = r.view(key);
    if (!v.view)
        return E_OUTOFMEMORY;
    v.format = r.format;
    v.aspects = r.aspects;
    v.width = std::max<uint32_t>(static_cast<uint32_t>(rd.Width >> key.mip), 1);
    v.height = std::max<uint32_t>(rd.Height >> key.mip, 1);
    v.mip = key.mip;
    v.first_layer = key.layer;
    v.layers = key.layers;
    v.flags = desc ? desc->Flags : D3D12_DSV_FLAG_NONE;
    *out = v;
    return S_OK;
}

} // namespace vgd
