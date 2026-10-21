// SPDX-License-Identifier: MIT
#include "internal.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace vgd {

// --- Allocators --------------------------------------------------------------------------------------------

CommandAllocator::Impl::~Impl()
{
    if (pool)
        device->vk.vkDestroyCommandPool(device->device, pool, nullptr);
}

HRESULT CommandAllocator::Impl::acquire(VkCommandBuffer* out)
{
    if (next < buffers.size()) {
        *out = buffers[next++];
        return S_OK;
    }
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VkResult r = device->vk.vkAllocateCommandBuffers(device->device, &ai, &cmd);
    if (r != VK_SUCCESS)
        return hresult_from_vk(r);
    buffers.push_back(cmd);
    next = buffers.size();
    *out = cmd;
    return S_OK;
}

CommandAllocator::CommandAllocator(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
CommandAllocator::~CommandAllocator() = default;

HRESULT CommandAllocator::reset()
{
    VkResult r = impl_->device->vk.vkResetCommandPool(impl_->device->device, impl_->pool, 0);
    impl_->next = 0;
    return hresult_from_vk(r);
}

HRESULT Device::create_command_allocator(D3D12_COMMAND_LIST_TYPE type, std::unique_ptr<CommandAllocator>* out)
{
    if (!out)
        return E_INVALIDARG;
    auto a = std::make_unique<CommandAllocator::Impl>();
    a->device = impl_.get();
    a->type = type;
    VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    ci.queueFamilyIndex = impl_->slot_for(type).family;
    VkResult r = impl_->vk.vkCreateCommandPool(impl_->device, &ci, nullptr, &a->pool);
    if (r != VK_SUCCESS)
        return hresult_from_vk(r);
    out->reset(new CommandAllocator(std::move(a)));
    return S_OK;
}

// --- Command lists -----------------------------------------------------------------------------------------

HRESULT CommandList::Impl::begin(CommandAllocator& allocator)
{
    if (allocator.impl().type != type)
        return E_INVALIDARG;
    HRESULT hr = allocator.impl().acquire(&cmd);
    if (FAILED(hr))
        return hr;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkResult r = device->vk.vkBeginCommandBuffer(cmd, &bi);
    if (r != VK_SUCCESS)
        return hresult_from_vk(r);
    open = true;
    error = S_OK;
    reset_state();
    return S_OK;
}

CommandList::CommandList(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
CommandList::~CommandList() = default;

HRESULT Device::create_command_list(D3D12_COMMAND_LIST_TYPE type, CommandAllocator& allocator,
                                    std::unique_ptr<CommandList>* out)
{
    if (!out)
        return E_INVALIDARG;
    auto l = std::make_unique<CommandList::Impl>();
    l->device = impl_.get();
    l->type = type;
    HRESULT hr = l->begin(allocator);
    if (FAILED(hr))
        return hr;
    out->reset(new CommandList(std::move(l)));
    return S_OK;
}

HRESULT Device::create_command_list(D3D12_COMMAND_LIST_TYPE type, std::unique_ptr<CommandList>* out)
{
    if (!out)
        return E_INVALIDARG;
    auto l = std::make_unique<CommandList::Impl>();
    l->device = impl_.get();
    l->type = type;
    out->reset(new CommandList(std::move(l)));
    return S_OK;
}

HRESULT CommandList::close()
{
    if (!impl_->open)
        return E_FAIL;
    impl_->end_rendering();
    impl_->end_active_queries();
    impl_->open = false;
    VkResult r = impl_->device->vk.vkEndCommandBuffer(impl_->cmd);
    return FAILED(impl_->error) ? impl_->error : hresult_from_vk(r);
}

HRESULT CommandList::reset(CommandAllocator& allocator)
{
    if (impl_->open)
        return E_FAIL;
    return impl_->begin(allocator);
}

void CommandList::copy_buffer_region(Resource& dst, uint64_t dst_offset, Resource& src, uint64_t src_offset,
                                     uint64_t bytes)
{
    impl_->end_rendering();
    VkBufferCopy region{src_offset, dst_offset, bytes};
    impl_->device->vk.vkCmdCopyBuffer(impl_->cmd, src.impl().buffer, dst.impl().buffer, 1, &region);
}

void CommandList::resource_barrier(std::span<const D3D12_RESOURCE_BARRIER> barriers)
{
    if (barriers.empty())
        return;
    // Every legacy barrier batch is a full memory dependency. Images never leave VK_IMAGE_LAYOUT_GENERAL, so
    // transitions need no layout changes. Split barriers: the begin half is dropped, the end half does the work.
    bool any = false;
    for (const auto& b : barriers)
        any |= !(b.Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY);
    if (!any)
        return;
    global_barrier();
}

void CommandList::global_barrier()
{
    impl_->end_rendering();
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    impl_->device->vk.vkCmdPipelineBarrier2(impl_->cmd, &dep);
}

// --- Texture copies ----------------------------------------------------------------------------------------

namespace {

// D3D12 subresource index -> Vulkan subresource layers (one mip, one layer, one aspect).
VkImageSubresourceLayers subresource_layers(const Resource::Impl& r, uint32_t sub)
{
    const uint32_t mips = r.desc.MipLevels, layers = r.array_layers();
    const uint32_t mip = sub % mips, layer = (sub / mips) % layers, plane = sub / (mips * layers);
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    if (r.aspects & VK_IMAGE_ASPECT_DEPTH_BIT)
        aspect = plane ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    else if (r.aspects == VK_IMAGE_ASPECT_STENCIL_BIT)
        aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
    return {aspect, mip, layer, 1};
}

VkExtent3D mip_extent(const Resource::Impl& r, uint32_t mip)
{
    const auto& d = r.desc;
    const uint32_t depth = d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? d.DepthOrArraySize : 1;
    return {std::max<uint32_t>(static_cast<uint32_t>(d.Width >> mip), 1), std::max<uint32_t>(d.Height >> mip, 1),
            std::max<uint32_t>(depth >> mip, 1)};
}

// A footprint's buffer position of texel (x, y, z), and its row length / image height in texels.
VkBufferImageCopy footprint_region(const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& f, VkFormat format,
                                   VkImageAspectFlags aspect, uint32_t x, uint32_t y, uint32_t z)
{
    const FormatBlock b = format_block(format, aspect);
    VkBufferImageCopy c{};
    const uint64_t rows_per_slice = (f.Footprint.Height + b.height - 1) / b.height;
    c.bufferOffset = f.Offset + uint64_t(z) * rows_per_slice * f.Footprint.RowPitch +
                     uint64_t(y / b.height) * f.Footprint.RowPitch + uint64_t(x / b.width) * b.bytes;
    c.bufferRowLength = b.bytes ? f.Footprint.RowPitch / b.bytes * b.width : 0;
    c.bufferImageHeight = static_cast<uint32_t>(rows_per_slice * b.height);
    return c;
}

} // namespace

void CommandList::copy_texture_region(const TextureCopyLocation& dst, uint32_t x, uint32_t y, uint32_t z,
                                      const TextureCopyLocation& src, const D3D12_BOX* box)
{
    Impl& l = *impl_;
    l.end_rendering();
    auto& vk = l.device->vk;
    if (!dst.resource || !src.resource || (dst.is_footprint && src.is_footprint)) {
        l.fail(E_INVALIDARG);
        return;
    }
    Resource::Impl& d = dst.resource->impl();
    Resource::Impl& s = src.resource->impl();

    if (dst.is_footprint || src.is_footprint) {
        const bool to_buffer = dst.is_footprint;
        Resource::Impl& image = to_buffer ? s : d;
        Resource::Impl& buffer = to_buffer ? d : s;
        const TextureCopyLocation& tex = to_buffer ? src : dst;
        const TextureCopyLocation& fp = to_buffer ? dst : src;
        if (!image.image || !buffer.buffer) {
            l.fail(E_INVALIDARG);
            return;
        }
        const VkImageSubresourceLayers layers = subresource_layers(image, tex.subresource);
        // The box selects from the source: texels of the image when reading it, texels of the footprint when
        // writing the image. (x, y, z) places the copy in the destination.
        const D3D12_BOX full = to_buffer
            ? D3D12_BOX{0, 0, 0, mip_extent(image, layers.mipLevel).width, mip_extent(image, layers.mipLevel).height,
                        mip_extent(image, layers.mipLevel).depth}
            : D3D12_BOX{0, 0, 0, fp.footprint.Footprint.Width, fp.footprint.Footprint.Height,
                        fp.footprint.Footprint.Depth};
        const D3D12_BOX b = box ? *box : full;
        VkBufferImageCopy c;
        if (to_buffer) {
            c = footprint_region(fp.footprint, image.format, layers.aspectMask, x, y, z);
            c.imageOffset = {static_cast<int32_t>(b.left), static_cast<int32_t>(b.top), static_cast<int32_t>(b.front)};
        } else {
            c = footprint_region(fp.footprint, image.format, layers.aspectMask, b.left, b.top, b.front);
            c.imageOffset = {static_cast<int32_t>(x), static_cast<int32_t>(y), static_cast<int32_t>(z)};
        }
        c.imageSubresource = layers;
        c.imageExtent = {b.right - b.left, b.bottom - b.top, b.back - b.front};
        if (to_buffer)
            vk.vkCmdCopyImageToBuffer(l.cmd, image.image, VK_IMAGE_LAYOUT_GENERAL, buffer.buffer, 1, &c);
        else
            vk.vkCmdCopyBufferToImage(l.cmd, buffer.buffer, image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        return;
    }

    if (!d.image || !s.image) {
        l.fail(E_INVALIDARG);
        return;
    }
    VkImageCopy c{};
    c.srcSubresource = subresource_layers(s, src.subresource);
    c.dstSubresource = subresource_layers(d, dst.subresource);
    const VkExtent3D e = mip_extent(s, c.srcSubresource.mipLevel);
    const D3D12_BOX b = box ? *box : D3D12_BOX{0, 0, 0, e.width, e.height, e.depth};
    c.srcOffset = {static_cast<int32_t>(b.left), static_cast<int32_t>(b.top), static_cast<int32_t>(b.front)};
    c.dstOffset = {static_cast<int32_t>(x), static_cast<int32_t>(y), static_cast<int32_t>(z)};
    c.extent = {b.right - b.left, b.bottom - b.top, b.back - b.front};
    vk.vkCmdCopyImage(l.cmd, s.image, VK_IMAGE_LAYOUT_GENERAL, d.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
}

void CommandList::copy_resource(Resource& dst, Resource& src)
{
    Impl& l = *impl_;
    Resource::Impl& d = dst.impl();
    Resource::Impl& s = src.impl();
    if (d.buffer && s.buffer) {
        copy_buffer_region(dst, 0, src, 0, std::min(d.size, s.size));
        return;
    }
    if (!d.image || !s.image || d.desc.MipLevels != s.desc.MipLevels) {
        l.fail(E_INVALIDARG);
        return;
    }
    l.end_rendering();
    std::vector<VkImageCopy> regions;
    for (uint32_t mip = 0; mip < s.desc.MipLevels; mip++) {
        VkImageCopy c{};
        c.srcSubresource = {s.aspects, mip, 0, s.array_layers()};
        c.dstSubresource = {d.aspects, mip, 0, d.array_layers()};
        c.extent = mip_extent(s, mip);
        regions.push_back(c);
    }
    l.device->vk.vkCmdCopyImage(l.cmd, s.image, VK_IMAGE_LAYOUT_GENERAL, d.image, VK_IMAGE_LAYOUT_GENERAL,
                                static_cast<uint32_t>(regions.size()), regions.data());
}

// --- Graphics state ----------------------------------------------------------------------------------------

void CommandList::Impl::reset_state()
{
    pso = nullptr;
    rendering_pso = nullptr;
    resource_heap = sampler_heap = nullptr;
    graphics_root = {};
    compute_root = {};
    topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    viewports.clear();
    scissors.clear();
    viewports_dirty = true;
    vertex_buffers.fill({});
    vertex_buffers_dirty = true;
    index_buffer = VK_NULL_HANDLE;
    index_buffer_dirty = false;
    num_rtvs = 0;
    dsv = {};
    rendering = false;
    active_queries.clear();
}

void CommandList::Impl::fail(HRESULT hr)
{
    if (SUCCEEDED(error))
        error = hr;
}

void CommandList::Impl::end_rendering()
{
    if (!rendering)
        return;
    device->vk.vkCmdEndRendering(cmd);
    rendering = false;
    rendering_pso = nullptr;
}

namespace {

bool same_attachments(const GraphicsPipelineDesc& a, const GraphicsPipelineDesc& b)
{
    return a.num_render_targets == b.num_render_targets && a.rtv_formats == b.rtv_formats &&
           a.dsv_format == b.dsv_format;
}

VkPrimitiveTopology vk_topology(D3D12_PRIMITIVE_TOPOLOGY t)
{
    switch (t) {
    case D3D_PRIMITIVE_TOPOLOGY_POINTLIST: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
    default:
        if (t >= D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST &&
            t <= D3D_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST)
            return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
        return VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
    }
}

bool is_strip(D3D12_PRIMITIVE_TOPOLOGY t)
{
    return t == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP || t == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP ||
           t == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ || t == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ;
}

} // namespace

bool CommandList::Impl::prepare_draw()
{
    auto& vk = device->vk;
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT || !pso || pso->is_compute() || !pso->compiled()) {
        fail(E_INVALIDARG);
        return false;
    }
    if (viewports.empty() || vk_topology(topology) == VK_PRIMITIVE_TOPOLOGY_MAX_ENUM)
        return false;   // nothing can be rasterized; D3D12 leaves this undefined, we draw nothing
    const GraphicsPipelineDesc& g = pso->graphics();
    const PipelineState::Impl& p = pso->impl();

    // Dynamic rendering with the pipeline's attachment count. Bound RTVs beyond it are ignored and missing ones
    // are null attachments, as in D3D12.
    if (rendering && rendering_pso != pso && !same_attachments(rendering_pso->graphics(), g))
        end_rendering();
    if (!rendering) {
        std::array<VkRenderingAttachmentInfo, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> colors{};
        VkRect2D area{{0, 0}, {UINT32_MAX, UINT32_MAX}};
        uint32_t layers = UINT32_MAX;
        for (uint32_t i = 0; i < g.num_render_targets; i++) {
            auto& a = colors[i];
            a.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            a.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            if (i < num_rtvs && rtvs[i].view) {
                a.imageView = rtvs[i].view;
                area.extent.width = std::min(area.extent.width, rtvs[i].width);
                area.extent.height = std::min(area.extent.height, rtvs[i].height);
                layers = std::min(layers, rtvs[i].layers);
            }
        }
        // The DSV is attached only to pipelines with a depth/stencil format: Vulkan requires the formats to match.
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingAttachmentInfo stencil = depth;
        const bool ds_pipeline = g.dsv_format != DXGI_FORMAT_UNKNOWN;
        if (ds_pipeline && dsv.view) {
            depth.imageView = stencil.imageView = dsv.view;
            area.extent.width = std::min(area.extent.width, dsv.width);
            area.extent.height = std::min(area.extent.height, dsv.height);
            layers = std::min(layers, dsv.layers);
        }
        if (area.extent.width == UINT32_MAX) {
            // No render targets: the viewports bound the area.
            area.extent = {1, 1};
            for (const VkViewport& v : viewports) {
                area.extent.width = std::max(area.extent.width, static_cast<uint32_t>(v.x + v.width));
                area.extent.height = std::max(area.extent.height, static_cast<uint32_t>(v.y));
            }
            layers = 1;
        }
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = area;
        ri.layerCount = layers;
        ri.colorAttachmentCount = g.num_render_targets;
        ri.pColorAttachments = colors.data();
        if (ds_pipeline) {
            const VkImageAspectFlags aspects = format_aspects(device->resolve_format(g.dsv_format));
            if (aspects & VK_IMAGE_ASPECT_DEPTH_BIT)
                ri.pDepthAttachment = &depth;
            if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT)
                ri.pStencilAttachment = &stencil;
        }
        vk.vkCmdBeginRendering(cmd, &ri);
        rendering = true;
        rendering_pso = pso;
    }

    const bool new_pipeline = graphics_root.bound != pso;
    flush_root(graphics_root, VK_PIPELINE_BIND_POINT_GRAPHICS, pso);
    if (viewports_dirty) {
        // Scissors pair with viewports; a missing one is the empty rect, as in D3D12.
        std::vector<VkRect2D> s(viewports.size(), VkRect2D{});
        std::copy_n(scissors.begin(), std::min(scissors.size(), s.size()), s.begin());
        vk.vkCmdSetViewportWithCount(cmd, static_cast<uint32_t>(viewports.size()), viewports.data());
        vk.vkCmdSetScissorWithCount(cmd, static_cast<uint32_t>(s.size()), s.data());
        viewports_dirty = false;
    }
    vk.vkCmdSetPrimitiveRestartEnable(cmd, p.primitive_restart && is_strip(topology));
    if (vertex_buffers_dirty || new_pipeline) {
        for (uint32_t slot = 0; slot < vertex_buffers.size(); slot++) {
            const VertexBuffer& vb = vertex_buffers[slot];
            if ((p.vertex_binding_mask & (1u << slot)) && vb.buffer)
                vk.vkCmdBindVertexBuffers2(cmd, slot, 1, &vb.buffer, &vb.offset, &vb.size, &vb.stride);
        }
        vertex_buffers_dirty = false;
    }
    if (index_buffer_dirty) {
        vk.vkCmdBindIndexBuffer(cmd, index_buffer, index_offset, index_type);
        index_buffer_dirty = false;
    }
    return true;
}

void CommandList::Impl::flush_root(RootState& root, VkPipelineBindPoint bind_point, const PipelineState* pipeline)
{
    auto& vk = device->vk;
    const PipelineState::Impl& p = pipeline->impl();
    const bool new_pipeline = root.bound != pipeline;
    if (new_pipeline) {
        vk.vkCmdBindPipeline(cmd, bind_point, p.pipeline);
        root.bound = pipeline;
    }
    if (root.heaps_dirty || new_pipeline) {
        // Pipeline layouts all start with the two heap sets, so the bound heaps survive pipeline changes in Vulkan
        // terms too; rebinding on a new pipeline also covers its static sampler set.
        if (resource_heap && resource_heap->set)
            vk.vkCmdBindDescriptorSets(cmd, bind_point, p.layout, kResourceHeapSet, 1, &resource_heap->set, 0,
                                       nullptr);
        if (sampler_heap && sampler_heap->set)
            vk.vkCmdBindDescriptorSets(cmd, bind_point, p.layout, kSamplerHeapSet, 1, &sampler_heap->set, 0,
                                       nullptr);
        if (p.static_set)
            vk.vkCmdBindDescriptorSets(cmd, bind_point, p.layout, kStaticSamplerSet, 1, &p.static_set, 0, nullptr);
        root.heaps_dirty = false;
    }
    if (p.push_size && (root.dirty || new_pipeline)) {
        vk.vkCmdPushConstants(cmd, p.layout, VK_SHADER_STAGE_ALL, 0, p.push_size, root.data.data());
        root.dirty = false;
    }
}

bool CommandList::Impl::prepare_dispatch()
{
    if (type == D3D12_COMMAND_LIST_TYPE_COPY || !pso || !pso->is_compute() || !pso->compiled()) {
        fail(E_INVALIDARG);
        return false;
    }
    end_rendering();
    flush_root(compute_root, VK_PIPELINE_BIND_POINT_COMPUTE, pso);
    return true;
}

void CommandList::set_pipeline_state(const PipelineState* pso)
{
    impl_->pso = pso;
}

namespace {

using RootState = CommandList::Impl::RootState;

void set_root_signature(RootState& root, const RootSignature* rs)
{
    if (root.rs == rs)
        return;
    root.rs = rs;   // a new root signature invalidates every root argument
    root.data.fill(0);
    root.dirty = true;
}

// The root parameter `index` of `type`, or null (and the list fails).
const RootSignature::Parameter* root_parameter(CommandList::Impl& l, RootState& root, uint32_t index,
                                               D3D12_ROOT_PARAMETER_TYPE type)
{
    if (!root.rs || index >= root.rs->parameters().size()) {
        l.fail(E_INVALIDARG);
        return nullptr;
    }
    const auto& p = root.rs->parameters()[index];
    const bool descriptor = type == D3D12_ROOT_PARAMETER_TYPE_CBV;   // stands for any root descriptor
    if (descriptor ? p.size != 8 : p.type != type) {
        l.fail(E_INVALIDARG);
        return nullptr;
    }
    return &p;
}

void set_root_constants(CommandList::Impl& l, RootState& root, uint32_t parameter, std::span<const uint32_t> values,
                        uint32_t dest_offset)
{
    const auto* p = root_parameter(l, root, parameter, D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS);
    if (!p)
        return;
    if (4 * (dest_offset + values.size()) > p->size) {
        l.fail(E_INVALIDARG);
        return;
    }
    std::memcpy(root.data.data() + p->offset + 4 * dest_offset, values.data(), 4 * values.size());
    root.dirty = true;
}

void set_root_descriptor(CommandList::Impl& l, RootState& root, uint32_t parameter, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    const auto* p = root_parameter(l, root, parameter, D3D12_ROOT_PARAMETER_TYPE_CBV);
    if (!p)
        return;
    std::memcpy(root.data.data() + p->offset, &address, 8);
    root.dirty = true;
}

void set_root_table(CommandList::Impl& l, RootState& root, uint32_t parameter, uint64_t gpu_handle)
{
    const auto* p = root_parameter(l, root, parameter, D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE);
    if (!p)
        return;
    DescriptorHeap::Impl* heap = p->sampler_table ? l.sampler_heap : l.resource_heap;
    const uint64_t base = heap ? reinterpret_cast<uint64_t>(heap->records.get()) : 0;
    if (!heap || gpu_handle < base || gpu_handle >= base + uint64_t(heap->count) * kDescriptorStride) {
        log(LogLevel::warn, "descriptor table %u: handle 0x%llx is not in the bound %s heap", parameter,
            static_cast<unsigned long long>(gpu_handle), p->sampler_table ? "sampler" : "CBV/SRV/UAV");
        l.fail(E_INVALIDARG);
        return;
    }
    // The table becomes its first descriptor's index in the heap, in the root data word dxil-spirv reads.
    const uint32_t index = static_cast<uint32_t>((gpu_handle - base) / kDescriptorStride);
    std::memcpy(root.data.data() + p->offset, &index, 4);
    root.dirty = true;
}

} // namespace

void CommandList::set_graphics_root_signature(const RootSignature* rs)
{
    set_root_signature(impl_->graphics_root, rs);
}

void CommandList::set_compute_root_signature(const RootSignature* rs)
{
    set_root_signature(impl_->compute_root, rs);
}

void CommandList::set_graphics_root_32bit_constants(uint32_t parameter, std::span<const uint32_t> values,
                                                    uint32_t dest_offset)
{
    set_root_constants(*impl_, impl_->graphics_root, parameter, values, dest_offset);
}

void CommandList::set_compute_root_32bit_constants(uint32_t parameter, std::span<const uint32_t> values,
                                                   uint32_t dest_offset)
{
    set_root_constants(*impl_, impl_->compute_root, parameter, values, dest_offset);
}

void CommandList::set_graphics_root_descriptor(uint32_t parameter, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    set_root_descriptor(*impl_, impl_->graphics_root, parameter, address);
}

void CommandList::set_compute_root_descriptor(uint32_t parameter, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    set_root_descriptor(*impl_, impl_->compute_root, parameter, address);
}

void CommandList::set_graphics_root_descriptor_table(uint32_t parameter, uint64_t gpu_handle)
{
    set_root_table(*impl_, impl_->graphics_root, parameter, gpu_handle);
}

void CommandList::set_compute_root_descriptor_table(uint32_t parameter, uint64_t gpu_handle)
{
    set_root_table(*impl_, impl_->compute_root, parameter, gpu_handle);
}

void CommandList::set_descriptor_heaps(std::span<DescriptorHeap* const> heaps)
{
    Impl& l = *impl_;
    l.resource_heap = l.sampler_heap = nullptr;
    for (DescriptorHeap* h : heaps) {
        if (!h)
            continue;
        if (h->type() == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
            l.sampler_heap = &h->impl();
        else
            l.resource_heap = &h->impl();
    }
    l.graphics_root.heaps_dirty = l.compute_root.heaps_dirty = true;
}

void CommandList::dispatch(uint32_t x, uint32_t y, uint32_t z)
{
    if (impl_->prepare_dispatch())
        impl_->device->vk.vkCmdDispatch(impl_->cmd, x, y, z);
}

void CommandList::set_primitive_topology(D3D12_PRIMITIVE_TOPOLOGY topology)
{
    Impl& l = *impl_;
    l.topology = topology;
    const VkPrimitiveTopology t = vk_topology(topology);
    if (l.type == D3D12_COMMAND_LIST_TYPE_DIRECT && t != VK_PRIMITIVE_TOPOLOGY_MAX_ENUM)
        l.device->vk.vkCmdSetPrimitiveTopology(l.cmd, t);
}

void CommandList::set_viewports(std::span<const D3D12_VIEWPORT> viewports)
{
    Impl& l = *impl_;
    l.viewports.clear();
    for (const auto& v : viewports) {
        // Flipped (negative height) so that Vulkan's framebuffer y runs the way D3D's does.
        l.viewports.push_back({v.TopLeftX, v.TopLeftY + v.Height, v.Width, -v.Height, v.MinDepth, v.MaxDepth});
    }
    l.viewports_dirty = true;
}

void CommandList::set_scissor_rects(std::span<const D3D12_RECT> rects)
{
    Impl& l = *impl_;
    l.scissors.clear();
    for (const auto& r : rects) {
        const LONG left = std::max<LONG>(r.left, 0), top = std::max<LONG>(r.top, 0);
        l.scissors.push_back({{left, top},
                              {static_cast<uint32_t>(std::max<LONG>(r.right - left, 0)),
                               static_cast<uint32_t>(std::max<LONG>(r.bottom - top, 0))}});
    }
    l.viewports_dirty = true;
}

void CommandList::set_blend_factor(const float factor[4])
{
    Impl& l = *impl_;
    if (l.type == D3D12_COMMAND_LIST_TYPE_DIRECT)
        l.device->vk.vkCmdSetBlendConstants(l.cmd, factor);
}

void CommandList::set_stencil_ref(uint32_t ref)
{
    Impl& l = *impl_;
    if (l.type == D3D12_COMMAND_LIST_TYPE_DIRECT)
        l.device->vk.vkCmdSetStencilReference(l.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, ref);
}

void CommandList::set_vertex_buffers(uint32_t start_slot, std::span<const D3D12_VERTEX_BUFFER_VIEW> views)
{
    Impl& l = *impl_;
    for (size_t i = 0; i < views.size() && start_slot + i < l.vertex_buffers.size(); i++) {
        Impl::VertexBuffer& vb = l.vertex_buffers[start_slot + i];
        vb = {};
        VkDeviceSize left = 0;
        if (views[i].BufferLocation)
            vb.buffer = l.device->find_buffer(views[i].BufferLocation, &vb.offset, &left);
        vb.size = std::min<VkDeviceSize>(views[i].SizeInBytes, left);
        vb.stride = views[i].StrideInBytes;
        if (views[i].BufferLocation && !vb.buffer)
            log(LogLevel::warn, "vertex buffer at 0x%llx is not in any buffer",
                static_cast<unsigned long long>(views[i].BufferLocation));
    }
    l.vertex_buffers_dirty = true;
}

void CommandList::set_index_buffer(const D3D12_INDEX_BUFFER_VIEW* view)
{
    Impl& l = *impl_;
    l.index_buffer = VK_NULL_HANDLE;
    if (view && view->BufferLocation) {
        VkDeviceSize left;
        l.index_buffer = l.device->find_buffer(view->BufferLocation, &l.index_offset, &left);
        l.index_type = view->Format == DXGI_FORMAT_R32_UINT ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    }
    l.index_buffer_dirty = l.index_buffer != VK_NULL_HANDLE;
}

void CommandList::set_render_targets(std::span<const RenderTargetView> rtvs, const DepthStencilView* dsv)
{
    Impl& l = *impl_;
    const uint32_t n = static_cast<uint32_t>(std::min(rtvs.size(), l.rtvs.size()));
    const VkImageView dsv_view = dsv ? dsv->view : VK_NULL_HANDLE;
    bool same = n == l.num_rtvs && dsv_view == l.dsv.view;
    for (uint32_t i = 0; same && i < n; i++)
        same = rtvs[i].view == l.rtvs[i].view;
    if (same)
        return;
    l.end_rendering();
    std::copy_n(rtvs.begin(), n, l.rtvs.begin());
    l.num_rtvs = n;
    l.dsv = dsv ? *dsv : DepthStencilView{};
}

void CommandList::clear_depth_stencil_view(const DepthStencilView& dsv, D3D12_CLEAR_FLAGS flags, float depth,
                                           uint8_t stencil, std::span<const D3D12_RECT> rects)
{
    Impl& l = *impl_;
    auto& vk = l.device->vk;
    l.end_rendering();
    if (!dsv.resource || !dsv.view) {
        l.fail(E_INVALIDARG);
        return;
    }
    VkImageAspectFlags aspects = 0;
    if (flags & D3D12_CLEAR_FLAG_DEPTH)
        aspects |= dsv.aspects & VK_IMAGE_ASPECT_DEPTH_BIT;
    if (flags & D3D12_CLEAR_FLAG_STENCIL)
        aspects |= dsv.aspects & VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!aspects)
        return;
    const VkClearDepthStencilValue value{depth, stencil};
    if (rects.empty()) {
        const VkImageSubresourceRange range{aspects, dsv.mip, 1, dsv.first_layer, dsv.layers};
        vk.vkCmdClearDepthStencilImage(l.cmd, dsv.resource->impl().image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
        return;
    }
    // Rects clear as an attachment, with every plane of the view attached (only the cleared ones are written).
    VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    a.imageView = dsv.view;
    a.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {dsv.width, dsv.height}};
    ri.layerCount = dsv.layers;
    if (dsv.aspects & VK_IMAGE_ASPECT_DEPTH_BIT)
        ri.pDepthAttachment = &a;
    if (dsv.aspects & VK_IMAGE_ASPECT_STENCIL_BIT)
        ri.pStencilAttachment = &a;
    std::vector<VkClearRect> clear_rects;
    for (const auto& rc : rects) {
        const LONG left = std::max<LONG>(rc.left, 0), top = std::max<LONG>(rc.top, 0);
        const LONG right = std::min<LONG>(rc.right, dsv.width), bottom = std::min<LONG>(rc.bottom, dsv.height);
        if (right > left && bottom > top)
            clear_rects.push_back({{{left, top}, {uint32_t(right - left), uint32_t(bottom - top)}}, 0, dsv.layers});
    }
    if (clear_rects.empty())
        return;
    VkClearAttachment ca{aspects, 0, {}};
    ca.clearValue.depthStencil = value;
    vk.vkCmdBeginRendering(l.cmd, &ri);
    vk.vkCmdClearAttachments(l.cmd, 1, &ca, static_cast<uint32_t>(clear_rects.size()), clear_rects.data());
    vk.vkCmdEndRendering(l.cmd);
}

void CommandList::set_depth_bounds(float min, float max)
{
    Impl& l = *impl_;
    if (l.type == D3D12_COMMAND_LIST_TYPE_DIRECT && l.device->physical->features.depthBounds)
        l.device->vk.vkCmdSetDepthBounds(l.cmd, min, max);
}

void CommandList::resolve_subresource(Resource& dst, uint32_t dst_subresource, Resource& src,
                                      uint32_t src_subresource, DXGI_FORMAT)
{
    Impl& l = *impl_;
    Resource::Impl& d = dst.impl();
    Resource::Impl& s = src.impl();
    if (!d.image || !s.image || s.aspects != VK_IMAGE_ASPECT_COLOR_BIT) {
        log(LogLevel::warn, "ResolveSubresource: only color images can be resolved");
        l.fail(E_INVALIDARG);
        return;
    }
    l.end_rendering();
    // Both images are read in their own formats. D3D12 resolves a typeless pair in the given format, which differs
    // only for sRGB averaging; that is not handled yet.
    VkImageResolve r{};
    r.srcSubresource = subresource_layers(s, src_subresource);
    r.dstSubresource = subresource_layers(d, dst_subresource);
    r.extent = mip_extent(s, r.srcSubresource.mipLevel);
    l.device->vk.vkCmdResolveImage(l.cmd, s.image, VK_IMAGE_LAYOUT_GENERAL, d.image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
}

void CommandList::clear_render_target_view(const RenderTargetView& rtv, const float color[4],
                                           std::span<const D3D12_RECT> rects)
{
    Impl& l = *impl_;
    auto& vk = l.device->vk;
    l.end_rendering();
    if (!rtv.resource || !rtv.view) {
        l.fail(E_INVALIDARG);
        return;
    }
    const Resource::Impl& r = rtv.resource->impl();
    VkClearColorValue value;
    if (is_integer(rtv.format)) {
        for (int i = 0; i < 4; i++)
            value.int32[i] = static_cast<int32_t>(color[i]);   // the same bits serve the unsigned formats
    } else {
        std::memcpy(value.float32, color, sizeof(value.float32));
    }
    // A whole view in the image's own format clears directly; rects, and views that reinterpret the format (an
    // sRGB view must encode the color), clear as an attachment.
    if (rects.empty() && rtv.format == r.format) {
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, rtv.mip, 1, rtv.first_layer, rtv.layers};
        vk.vkCmdClearColorImage(l.cmd, r.image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
        return;
    }
    VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    a.imageView = rtv.view;
    a.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {rtv.width, rtv.height}};
    ri.layerCount = rtv.layers;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &a;
    std::vector<VkClearRect> clear_rects;
    for (const auto& rc : rects) {
        const LONG left = std::max<LONG>(rc.left, 0), top = std::max<LONG>(rc.top, 0);
        const LONG right = std::min<LONG>(rc.right, rtv.width), bottom = std::min<LONG>(rc.bottom, rtv.height);
        if (right > left && bottom > top)
            clear_rects.push_back({{{left, top}, {uint32_t(right - left), uint32_t(bottom - top)}}, 0, rtv.layers});
    }
    if (rects.empty())
        clear_rects.push_back({ri.renderArea, 0, rtv.layers});
    if (clear_rects.empty())
        return;
    VkClearAttachment ca{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
    ca.clearValue.color = value;
    vk.vkCmdBeginRendering(l.cmd, &ri);
    vk.vkCmdClearAttachments(l.cmd, 1, &ca, static_cast<uint32_t>(clear_rects.size()), clear_rects.data());
    vk.vkCmdEndRendering(l.cmd);
}

void CommandList::draw_instanced(uint32_t vertex_count, uint32_t instance_count, uint32_t start_vertex,
                                 uint32_t start_instance)
{
    if (impl_->prepare_draw())
        impl_->device->vk.vkCmdDraw(impl_->cmd, vertex_count, instance_count, start_vertex, start_instance);
}

void CommandList::draw_indexed_instanced(uint32_t index_count, uint32_t instance_count, uint32_t start_index,
                                         int32_t base_vertex, uint32_t start_instance)
{
    Impl& l = *impl_;
    if (!l.index_buffer) {
        l.fail(E_INVALIDARG);
        return;
    }
    if (l.prepare_draw())
        l.device->vk.vkCmdDrawIndexed(l.cmd, index_count, instance_count, start_index, base_vertex, start_instance);
}

// --- Indirect ----------------------------------------------------------------------------------------------

HRESULT CommandSignature::create(const D3D12_COMMAND_SIGNATURE_DESC& desc, CommandSignature* out)
{
    if (desc.NumArgumentDescs != 1 || !desc.pArgumentDescs) {
        log(LogLevel::warn, "command signature with %u arguments: only single draw/dispatch arguments are supported",
            desc.NumArgumentDescs);
        return E_NOTIMPL;
    }
    uint32_t size;
    switch (desc.pArgumentDescs[0].Type) {
    case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW: size = sizeof(D3D12_DRAW_ARGUMENTS); break;
    case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED: size = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS); break;
    case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH: size = sizeof(D3D12_DISPATCH_ARGUMENTS); break;
    default:
        log(LogLevel::warn, "command signature argument type %d is not supported",
            static_cast<int>(desc.pArgumentDescs[0].Type));
        return E_NOTIMPL;
    }
    if (desc.ByteStride < size || desc.ByteStride % 4)
        return E_INVALIDARG;
    out->type = desc.pArgumentDescs[0].Type;
    out->stride = desc.ByteStride;
    return S_OK;
}

// D3D12's argument structures are Vulkan's indirect commands, field for field.
static_assert(sizeof(D3D12_DRAW_ARGUMENTS) == sizeof(VkDrawIndirectCommand));
static_assert(sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) == sizeof(VkDrawIndexedIndirectCommand));
static_assert(sizeof(D3D12_DISPATCH_ARGUMENTS) == sizeof(VkDispatchIndirectCommand));

void CommandList::execute_indirect(const CommandSignature& signature, uint32_t max_count, Resource& args,
                                   uint64_t args_offset, Resource* count, uint64_t count_offset)
{
    Impl& l = *impl_;
    auto& vk = l.device->vk;
    const Resource::Impl& a = args.impl();
    const Resource::Impl* c = count ? &count->impl() : nullptr;
    const uint64_t arg_size = signature.type == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW ? sizeof(D3D12_DRAW_ARGUMENTS)
                              : signature.type == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED
                                  ? sizeof(D3D12_DRAW_INDEXED_ARGUMENTS)
                                  : sizeof(D3D12_DISPATCH_ARGUMENTS);
    if (!a.buffer || args_offset % 4 || (c && (!c->buffer || count_offset % 4 || count_offset + 4 > c->size)) ||
        (max_count && args_offset + uint64_t(max_count - 1) * signature.stride + arg_size > a.size)) {
        l.fail(E_INVALIDARG);
        return;
    }
    if (!max_count)
        return;
    const bool indexed = signature.type == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    if (signature.type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH) {
        if (c) {
            // Needs the count applied on the GPU first (a compute pass into scratch memory): not yet.
            log(LogLevel::warn, "ExecuteIndirect: dispatches with a count buffer are not supported");
            l.fail(E_NOTIMPL);
            return;
        }
        if (!l.prepare_dispatch())
            return;
        for (uint32_t i = 0; i < max_count; i++)
            vk.vkCmdDispatchIndirect(l.cmd, a.buffer, args_offset + uint64_t(i) * signature.stride);
        return;
    }
    if (indexed && !l.index_buffer) {
        l.fail(E_INVALIDARG);
        return;
    }
    if (!l.prepare_draw())
        return;
    if (c) {
        if (!l.device->physical->v12.drawIndirectCount) {
            log(LogLevel::warn, "ExecuteIndirect: no drawIndirectCount for a count buffer");
            l.fail(E_NOTIMPL);
            return;
        }
        (indexed ? vk.vkCmdDrawIndexedIndirectCount : vk.vkCmdDrawIndirectCount)(
            l.cmd, a.buffer, args_offset, c->buffer, count_offset, max_count, signature.stride);
        return;
    }
    // One call for all commands when the device can draw several.
    const auto draw = indexed ? vk.vkCmdDrawIndexedIndirect : vk.vkCmdDrawIndirect;
    if (l.device->physical->features.multiDrawIndirect &&
        max_count <= l.device->physical->limits.maxDrawIndirectCount) {
        draw(l.cmd, a.buffer, args_offset, max_count, signature.stride);
        return;
    }
    for (uint32_t i = 0; i < max_count; i++)
        draw(l.cmd, a.buffer, args_offset + uint64_t(i) * signature.stride, 1, signature.stride);
}

// --- UAV clears --------------------------------------------------------------------------------------------

namespace {

bool is_float32(VkFormat f)
{
    return f == VK_FORMAT_R32_SFLOAT || f == VK_FORMAT_R32G32_SFLOAT || f == VK_FORMAT_R32G32B32_SFLOAT ||
           f == VK_FORMAT_R32G32B32A32_SFLOAT;
}

bool is_unorm8(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R8G8_UNORM:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_UNORM: return true;
    default: return false;
    }
}

// Clears what a UAV descriptor covers. `bits` is the 32-bit pattern a buffer fill writes; `color` the value an
// image clear uses, already in the image format's terms.
void clear_uav(CommandList::Impl& l, const Descriptor& uav, uint32_t bits, const VkClearColorValue& color)
{
    auto& vk = l.device->vk;
    l.end_rendering();
    switch (uav.kind) {
    case Descriptor::Kind::storage_buffer:
        if (uav.buffer.buffer)
            vk.vkCmdFillBuffer(l.cmd, uav.buffer.buffer, uav.buffer.offset, uav.buffer.range & ~VkDeviceSize(3), bits);
        return;
    case Descriptor::Kind::storage_texel:
        if (!uav.texel.view)
            return;
        if (format_block(uav.texel.format).bytes != 4) {
            log(LogLevel::warn, "UAV clear of a typed buffer with format %d: only 32-bit formats so far",
                uav.texel.format);
            l.fail(E_NOTIMPL);
            return;
        }
        vk.vkCmdFillBuffer(l.cmd, uav.texel.buffer, uav.texel.offset, uav.texel.range & ~VkDeviceSize(3), bits);
        return;
    case Descriptor::Kind::storage_image: {
        if (!uav.image.view)
            return;
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, uav.image.mip, 1, uav.image.first_layer,
                                            uav.image.layers};
        vk.vkCmdClearColorImage(l.cmd, uav.image.resource->impl().image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
        return;
    }
    default:
        l.fail(E_INVALIDARG);
        return;
    }
}

VkFormat uav_format(const Descriptor& uav)
{
    return uav.kind == Descriptor::Kind::storage_image ? uav.image.format
         : uav.kind == Descriptor::Kind::storage_texel ? uav.texel.format
                                                        : VK_FORMAT_R32_UINT;
}

} // namespace

void CommandList::clear_unordered_access_view_uint(const Descriptor& uav, const uint32_t values[4])
{
    const VkFormat f = uav_format(uav);
    VkClearColorValue color{};
    if (is_integer(f)) {
        std::memcpy(color.uint32, values, 16);
    } else {
        // D3D12 writes the integers' bits into non-integer formats: for 8-bit UNORM that is v / 255, for 32-bit
        // floats the bit pattern. Other formats get the plain value (approximate).
        for (int i = 0; i < 4; i++)
            color.float32[i] = is_unorm8(f) ? float(values[i] & 0xff) / 255.0f
                               : is_float32(f)  ? std::bit_cast<float>(values[i])
                                                : float(values[i]);
    }
    clear_uav(*impl_, uav, values[0], color);
}

void CommandList::clear_unordered_access_view_float(const Descriptor& uav, const float values[4])
{
    const VkFormat f = uav_format(uav);
    VkClearColorValue color{};
    if (is_integer(f)) {
        for (int i = 0; i < 4; i++)
            color.uint32[i] = static_cast<uint32_t>(values[i]);
    } else {
        std::memcpy(color.float32, values, 16);
    }
    // A float clear of a buffer writes the value converted to the view's 32-bit format.
    const uint32_t bits = is_integer(f) ? static_cast<uint32_t>(values[0]) : std::bit_cast<uint32_t>(values[0]);
    clear_uav(*impl_, uav, bits, color);
}

// --- Queues ------------------------------------------------------------------------------------------------

CommandQueue::CommandQueue(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
CommandQueue::~CommandQueue() = default;

HRESULT Device::create_command_queue(const D3D12_COMMAND_QUEUE_DESC& desc, std::unique_ptr<CommandQueue>* out)
{
    if (!out)
        return E_INVALIDARG;
    if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT && desc.Type != D3D12_COMMAND_LIST_TYPE_COMPUTE &&
        desc.Type != D3D12_COMMAND_LIST_TYPE_COPY)
        return E_NOTIMPL;
    auto q = std::make_unique<CommandQueue::Impl>();
    q->device = impl_.get();
    q->slot = &impl_->slot_for(desc.Type);
    q->desc = desc;
    out->reset(new CommandQueue(std::move(q)));
    return S_OK;
}

HRESULT CommandQueue::Impl::submit(std::span<const VkCommandBufferSubmitInfo> cmds, const VkSemaphoreSubmitInfo* signal)
{
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    std::lock_guard lock(slot->submit_lock);
    // Images created since this queue's last submission must have been initialized first.
    const uint64_t init = device->init_value.load();
    if (init > init_waited) {
        VkSemaphoreSubmitInfo w{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        w.semaphore = device->init_timeline;
        w.value = init;
        w.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        pending_waits.push_back(w);
        init_waited = init;
    }
    si.waitSemaphoreInfoCount = static_cast<uint32_t>(pending_waits.size());
    si.pWaitSemaphoreInfos = pending_waits.data();
    si.commandBufferInfoCount = static_cast<uint32_t>(cmds.size());
    si.pCommandBufferInfos = cmds.data();
    si.signalSemaphoreInfoCount = signal ? 1 : 0;
    si.pSignalSemaphoreInfos = signal;
    VkResult r = device->vk.vkQueueSubmit2(slot->queue, 1, &si, VK_NULL_HANDLE);
    pending_waits.clear();
    return hresult_from_vk(r);
}

HRESULT CommandQueue::capture(std::span<CommandList* const> lists, CommandBatch* out)
{
    out->buffers.clear();
    out->buffers.reserve(lists.size());
    for (CommandList* l : lists) {
        if (!l || l->impl().open || !l->impl().cmd)
            return E_INVALIDARG;
        out->buffers.push_back(l->impl().cmd);
    }
    return S_OK;
}

HRESULT CommandQueue::execute_command_lists(std::span<CommandList* const> lists, Fence* signal, uint64_t value)
{
    CommandBatch batch;
    HRESULT hr = capture(lists, &batch);
    return FAILED(hr) ? hr : submit(batch, signal, value);
}

HRESULT CommandQueue::submit(const CommandBatch& batch, Fence* signal, uint64_t value)
{
    std::vector<VkCommandBufferSubmitInfo> cmds;
    cmds.reserve(batch.buffers.size());
    for (VkCommandBuffer cmd : batch.buffers) {
        VkCommandBufferSubmitInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        ci.commandBuffer = cmd;
        cmds.push_back(ci);
    }
    if (!signal)
        return impl_->submit(cmds, nullptr);
    VkSemaphoreSubmitInfo s{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    s.semaphore = signal->impl().timeline;
    s.value = value;
    s.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    return impl_->submit(cmds, &s);
}

HRESULT CommandQueue::signal(Fence& fence, uint64_t value)
{
    VkSemaphoreSubmitInfo s{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    s.semaphore = fence.impl().timeline;
    s.value = value;
    s.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    return impl_->submit({}, &s);
}

HRESULT CommandQueue::wait(Fence& fence, uint64_t value)
{
    // A Vulkan semaphore wait only orders its own batch, so it rides on this queue's next submission.
    // That submission (and, as drivers process a queue in order, everything after it) then waits.
    VkSemaphoreSubmitInfo w{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    w.semaphore = fence.impl().timeline;
    w.value = value;
    w.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    std::lock_guard lock(impl_->slot->submit_lock);
    impl_->pending_waits.push_back(w);
    return S_OK;
}

} // namespace vgd
