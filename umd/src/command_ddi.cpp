// SPDX-License-Identifier: MIT
// Command pools, recorders, lists and queues (the DDI model since 0040):
//  - a pool is untyped; it owns one core allocator per queue type, created when first needed;
//  - a recorder (the API's allocator, as the runtime sees it) has a queue type and a target pool;
//  - ResetCommandList names a recorder; the list then records into the target pool's allocator for its type.
#include "device.h"
#include "log.h"
#include "objects.h"
#include "queue_bridge.h"
#include "stubs.h"

#include <array>
#include <new>

namespace vgd::umd {

void CommandListObject::error(HRESULT hr) const
{
    VGD_WARN("command list error 0x%08lx (SetCommandListErrorCb %p)", hr,
             reinterpret_cast<void*>(device->um_callbacks->pfnSetCommandListErrorCb));
    device->um_callbacks->pfnSetCommandListErrorCb(rt, hr);
}

namespace {

constexpr size_t kQueueTypes = 3;   // D3D12_COMMAND_LIST_TYPE_DIRECT, COMPUTE, COPY map to slots 0..2

bool queue_type(D3D12DDI_COMMAND_QUEUE_FLAGS flags, D3D12_COMMAND_LIST_TYPE* type)
{
    if (flags & D3D12DDI_COMMAND_QUEUE_FLAG_3D)
        *type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    else if (flags & D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE)
        *type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    else if (flags & D3D12DDI_COMMAND_QUEUE_FLAG_COPY)
        *type = D3D12_COMMAND_LIST_TYPE_COPY;
    else
        return false;
    return true;
}

size_t slot_of(D3D12_COMMAND_LIST_TYPE type)
{
    return type == D3D12_COMMAND_LIST_TYPE_COMPUTE ? 1 : type == D3D12_COMMAND_LIST_TYPE_COPY ? 2 : 0;
}

struct CommandPoolObject {
    Device* device;
    std::array<std::unique_ptr<vgd::CommandAllocator>, kQueueTypes> allocators;
};

struct CommandRecorderObject {
    D3D12_COMMAND_LIST_TYPE type;
    CommandPoolObject* pool = nullptr;
};

struct CommandQueueObject {
    std::unique_ptr<vgd::CommandQueue> queue;
    std::unique_ptr<QueueBridge> bridge;   // declared last, so destroyed first: its threads use the queue
};

// --- Pools ---------------------------------------------------------------------------------------------------

SIZE_T APIENTRY calc_private_command_pool_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_COMMAND_POOL_0040*)
{
    return sizeof(CommandPoolObject);
}

HRESULT APIENTRY create_command_pool(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_COMMAND_POOL_0040*,
                                     D3D12DDI_HCOMMANDPOOL_0040 hp)
{
    new (hp.pDrvPrivate) CommandPoolObject{Device::from(h), {}};
    return S_OK;
}

void APIENTRY destroy_command_pool(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDPOOL_0040 hp)
{
    object<CommandPoolObject>(hp)->~CommandPoolObject();
}

void APIENTRY reset_command_pool(D3D12DDI_HDEVICE h, D3D12DDI_HCOMMANDPOOL_0040 hp)
{
    for (auto& a : object<CommandPoolObject>(hp)->allocators)
        if (a && FAILED(a->reset())) {
            Device* d = Device::from(h);
            d->um_callbacks->pfnSetErrorCb(d->rt_device, E_OUTOFMEMORY);
        }
}

// --- Recorders -----------------------------------------------------------------------------------------------

SIZE_T APIENTRY calc_private_command_recorder_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_COMMAND_RECORDER_0040*)
{
    return sizeof(CommandRecorderObject);
}

HRESULT APIENTRY create_command_recorder(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_COMMAND_RECORDER_0040* args,
                                         D3D12DDI_HCOMMANDRECORDER_0040 hr_)
{
    D3D12_COMMAND_LIST_TYPE type;
    if (!queue_type(args->QueueFlags, &type)) {
        VGD_WARN("CreateCommandRecorder: unsupported queue flags 0x%x", static_cast<unsigned>(args->QueueFlags));
        return E_OUTOFMEMORY;
    }
    new (hr_.pDrvPrivate) CommandRecorderObject{type};
    return S_OK;
}

void APIENTRY destroy_command_recorder(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDRECORDER_0040 hr_)
{
    object<CommandRecorderObject>(hr_)->~CommandRecorderObject();
}

void APIENTRY set_command_pool_as_target(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDRECORDER_0040 hr_,
                                         D3D12DDI_HCOMMANDPOOL_0040 hp)
{
    object<CommandRecorderObject>(hr_)->pool = hp.pDrvPrivate ? object<CommandPoolObject>(hp) : nullptr;
}

// --- Lists ---------------------------------------------------------------------------------------------------

SIZE_T APIENTRY calc_private_command_list_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_COMMAND_LIST_0040*)
{
    return sizeof(CommandListObject);
}

HRESULT APIENTRY create_command_list(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_COMMAND_LIST_0040* args,
                                     D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HRTCOMMANDLIST rt)
{
    D3D12_COMMAND_LIST_TYPE type;
    if (args->Type == D3D12DDI_COMMAND_LIST_TYPE_BUNDLE || !queue_type(args->QueueFlags, &type)) {
        VGD_WARN("CreateCommandList: unsupported type %d / queue flags 0x%x (bundles come later)",
                 static_cast<int>(args->Type), static_cast<unsigned>(args->QueueFlags));
        return E_OUTOFMEMORY;
    }
    Device* d = Device::from(h);
    // Bind the list to one of our command-list tables. They are identical; like Microsoft's sample driver, direct
    // lists take table 1 when the runtime filled it, compute and copy lists table 0.
    const CommandListTables& tables = *d->command_list_tables;
    const UINT index = type == D3D12_COMMAND_LIST_TYPE_DIRECT && tables.count > 1 ? 1 : 0;
    if (!tables.count || !tables.rt[index].handle) {
        VGD_WARN("CreateCommandList: the runtime filled no command list table");
        return E_OUTOFMEMORY;
    }
    d->um_callbacks->pfnSetCommandListDDITableCb(rt, tables.rt[index]);
    auto* obj = new (hl.pDrvPrivate) CommandListObject{d, rt, type, nullptr};
    if (FAILED(d->core->create_command_list(type, &obj->list))) {
        obj->~CommandListObject();
        return E_OUTOFMEMORY;
    }
    return S_OK;
}

void APIENTRY destroy_command_list(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDLIST hl)
{
    object<CommandListObject>(hl)->~CommandListObject();
}

void APIENTRY close_command_list(D3D12DDI_HCOMMANDLIST hl)
{
    auto* l = object<CommandListObject>(hl);
    HRESULT hr = l->list->close();
    VGD_VERBOSE("CloseCommandList: 0x%08lx", hr);
    if (FAILED(hr))
        l->error(E_FAIL);
}

void APIENTRY reset_command_list(D3D12DDI_HCOMMANDLIST hl, const D3D12DDIARG_RESETCOMMANDLIST_0040* args)
{
    auto* l = object<CommandListObject>(hl);
    auto* rec = object<CommandRecorderObject>(args->hDrvCommandRecorder);
    if (!rec->pool) {
        VGD_WARN("ResetCommandList: recorder has no target pool");
        l->error(E_INVALIDARG);
        return;
    }
    auto& alloc = rec->pool->allocators[slot_of(l->type)];
    if (!alloc && FAILED(l->device->core->create_command_allocator(l->type, &alloc))) {
        l->error(E_OUTOFMEMORY);
        return;
    }
    if (FAILED(l->list->reset(*alloc)))
        l->error(E_OUTOFMEMORY);
}

// --- Copies and barriers -------------------------------------------------------------------------------------

void APIENTRY copy_buffer_region(D3D12DDI_HCOMMANDLIST hl, D3D12DDIARG_BUFFER_PLACEMENT dst,
                                 D3D12DDIARG_BUFFER_PLACEMENT src, UINT64 bytes)
{
    auto* l = object<CommandListObject>(hl);
    auto* d = object<ResourceObject>(dst.BaseAddress.UMD.hResource);
    auto* s = object<ResourceObject>(src.BaseAddress.UMD.hResource);
    if (!d->resource || !s->resource || d->resource->is_texture() || s->resource->is_texture()) {
        l->error(E_INVALIDARG);
        return;
    }
    l->list->copy_buffer_region(*d->resource, dst.BaseAddress.UMD.Offset, *s->resource, src.BaseAddress.UMD.Offset, bytes);
}

void APIENTRY resource_copy(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HRESOURCE dst, D3D12DDI_HRESOURCE src)
{
    auto* l = object<CommandListObject>(hl);
    auto* d = object<ResourceObject>(dst);
    auto* s = object<ResourceObject>(src);
    if (!d->resource || !s->resource) {
        l->error(E_INVALIDARG);
        return;
    }
    l->list->copy_resource(*d->resource, *s->resource);
}

void APIENTRY resource_barrier(D3D12DDI_HCOMMANDLIST hl, UINT count, const D3D12DDIARG_RESOURCE_BARRIER_0022*)
{
    // Any batch of legacy barriers is one full memory dependency; images stay in VK_IMAGE_LAYOUT_GENERAL.
    if (count)
        object<CommandListObject>(hl)->list->global_barrier();
}

// --- Texture copies ------------------------------------------------------------------------------------------

// One side of CopyTextureRegion. A texture names its subresource in the placement offset; a buffer side is a pitched
// footprint whose dimensions the DDI gives in format blocks.
bool copy_location(const D3D12DDIARG_BUFFER_PLACEMENT& placement, const D3D12DDIARG_PLACED_RESOURCE& layout,
                   vgd::TextureCopyLocation* out)
{
    auto* r = object<ResourceObject>(placement.BaseAddress.UMD.hResource);
    if (!r || !r->resource)
        return false;
    out->resource = r->resource;
    switch (layout.Layout) {
    case D3D12DDI_RL_SELECT_SUBRESOURCE:
        out->is_footprint = false;
        out->subresource = static_cast<uint32_t>(placement.BaseAddress.UMD.Offset);
        return true;
    case D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED: {
        const auto& p = *static_cast<const D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT*>(layout.pLayout);
        const vgd::FormatBlock b = vgd::format_block(vgd::vk_format(p.Format));
        out->is_footprint = true;
        out->footprint.Offset = placement.BaseAddress.UMD.Offset;
        out->footprint.Footprint = {p.Format, p.PhysicalWidth * b.width, p.PhysicalHeight * b.height, p.PhysicalDepth,
                                    p.Pitch};
        if (p.PhysicalDepth > 1 && p.SlicePitch != p.Pitch * p.PhysicalHeight)
            VGD_WARN("CopyTextureRegion: slice pitch %u is not rows * pitch (%u * %u)", p.SlicePitch, p.PhysicalHeight,
                     p.Pitch);
        return true;
    }
    default:
        VGD_WARN("CopyTextureRegion: resource layout %d not supported", static_cast<int>(layout.Layout));
        return false;
    }
}

void APIENTRY copy_texture_region(D3D12DDI_HCOMMANDLIST hl, const D3D12DDIARG_BUFFER_PLACEMENT* dst,
                                  D3D12DDIARG_PLACED_RESOURCE dst_layout, UINT x, UINT y, UINT z,
                                  const D3D12DDIARG_BUFFER_PLACEMENT* src, D3D12DDIARG_PLACED_RESOURCE src_layout,
                                  const D3D12DDI_BOX* box)
{
    auto* l = object<CommandListObject>(hl);
    vgd::TextureCopyLocation d, s;
    if (!copy_location(*dst, dst_layout, &d) || !copy_location(*src, src_layout, &s)) {
        l->error(E_INVALIDARG);
        return;
    }
    D3D12_BOX b{};
    if (box)
        b = {static_cast<UINT>(box->Left), static_cast<UINT>(box->Top), static_cast<UINT>(box->Front),
             static_cast<UINT>(box->Right), static_cast<UINT>(box->Bottom), static_cast<UINT>(box->Back)};
    l->list->copy_texture_region(d, x, y, z, s, box ? &b : nullptr);
}

// --- Graphics state ------------------------------------------------------------------------------------------
// The DDI's viewport, rect, box and buffer-view structs are the API's field for field.
static_assert(sizeof(D3D12DDI_VIEWPORT) == sizeof(D3D12_VIEWPORT));
static_assert(sizeof(D3D12DDI_VERTEX_BUFFER_VIEW) == sizeof(D3D12_VERTEX_BUFFER_VIEW));
static_assert(sizeof(D3D12DDI_INDEX_BUFFER_VIEW) == sizeof(D3D12_INDEX_BUFFER_VIEW));

vgd::CommandList& list_of(D3D12DDI_HCOMMANDLIST hl)
{
    return *object<CommandListObject>(hl)->list;
}

void APIENTRY set_pipeline_state(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HPIPELINESTATE hp)
{
    list_of(hl).set_pipeline_state(hp.pDrvPrivate ? object<PipelineStateObject>(hp)->pso.get() : nullptr);
}

void APIENTRY set_graphics_root_signature(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HROOTSIGNATURE hr_)
{
    list_of(hl).set_graphics_root_signature(hr_.pDrvPrivate ? object<RootSignatureObject>(hr_)->rs.get() : nullptr);
}

void APIENTRY set_graphics_root_32bit_constant(D3D12DDI_HCOMMANDLIST hl, UINT index, UINT value, UINT offset)
{
    list_of(hl).set_graphics_root_32bit_constants(index, {&value, 1}, offset);
}

void APIENTRY set_graphics_root_32bit_constants(D3D12DDI_HCOMMANDLIST hl, UINT index, UINT count, const void* data,
                                                UINT offset)
{
    list_of(hl).set_graphics_root_32bit_constants(index, {static_cast<const uint32_t*>(data), count}, offset);
}

void APIENTRY set_graphics_root_buffer_view(D3D12DDI_HCOMMANDLIST hl, UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS va)
{
    list_of(hl).set_graphics_root_descriptor(index, va);
}

void APIENTRY set_descriptor_heaps(D3D12DDI_HCOMMANDLIST hl, UINT count, D3D12DDI_HDESCRIPTORHEAP* heaps)
{
    std::array<vgd::DescriptorHeap*, 2> core_heaps{};
    count = std::min<UINT>(count, core_heaps.size());
    for (UINT i = 0; i < count; i++)
        core_heaps[i] = heaps[i].pDrvPrivate ? object<DescriptorHeapObject>(heaps[i])->heap.get() : nullptr;
    list_of(hl).set_descriptor_heaps({core_heaps.data(), count});
}

void APIENTRY set_graphics_root_descriptor_table(D3D12DDI_HCOMMANDLIST hl, UINT index, D3D12DDI_GPU_DESCRIPTOR_HANDLE base)
{
    list_of(hl).set_graphics_root_descriptor_table(index, base.ptr);
}

void APIENTRY clear_root_arguments(D3D12DDI_HCOMMANDLIST hl)
{
    list_of(hl).set_graphics_root_signature(nullptr);
}

void APIENTRY ia_set_topology(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_PRIMITIVE_TOPOLOGY t)
{
    list_of(hl).set_primitive_topology(static_cast<D3D12_PRIMITIVE_TOPOLOGY>(t));
}

void APIENTRY rs_set_viewports(D3D12DDI_HCOMMANDLIST hl, UINT count, const D3D12DDI_VIEWPORT* v)
{
    list_of(hl).set_viewports({reinterpret_cast<const D3D12_VIEWPORT*>(v), count});
}

void APIENTRY rs_set_scissor_rects(D3D12DDI_HCOMMANDLIST hl, UINT count, const D3D12DDI_RECT* r)
{
    list_of(hl).set_scissor_rects({r, count});
}

void APIENTRY om_set_blend_factor(D3D12DDI_HCOMMANDLIST hl, const FLOAT factor[4])
{
    list_of(hl).set_blend_factor(factor);
}

void APIENTRY om_set_stencil_ref(D3D12DDI_HCOMMANDLIST hl, UINT ref)
{
    list_of(hl).set_stencil_ref(ref);
}

void APIENTRY ia_set_vertex_buffers(D3D12DDI_HCOMMANDLIST hl, UINT start, UINT count,
                                    const D3D12DDI_VERTEX_BUFFER_VIEW* views)
{
    std::array<D3D12_VERTEX_BUFFER_VIEW, D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> unbound{};
    const auto* v = views ? reinterpret_cast<const D3D12_VERTEX_BUFFER_VIEW*>(views) : unbound.data();
    list_of(hl).set_vertex_buffers(start, {v, std::min<size_t>(count, unbound.size())});
}

void APIENTRY ia_set_index_buffer(D3D12DDI_HCOMMANDLIST hl, const D3D12DDI_INDEX_BUFFER_VIEW* view)
{
    list_of(hl).set_index_buffer(reinterpret_cast<const D3D12_INDEX_BUFFER_VIEW*>(view));
}

void APIENTRY om_set_render_targets(D3D12DDI_HCOMMANDLIST hl, UINT count, const D3D12DDI_CPU_DESCRIPTOR_HANDLE* rtvs,
                                    BOOL single_range, const D3D12DDI_CPU_DESCRIPTOR_HANDLE* dsv)
{
    std::array<vgd::RenderTargetView, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> views{};
    count = std::min<UINT>(count, views.size());
    for (UINT i = 0; i < count; i++) {
        const D3D12DDI_CPU_DESCRIPTOR_HANDLE h =
            single_range ? D3D12DDI_CPU_DESCRIPTOR_HANDLE{rtvs[0].ptr + SIZE_T(i) * kDescriptorSize} : rtvs[i];
        const vgd::Descriptor* d = h.ptr ? descriptor_at(h) : nullptr;
        if (d && d->kind == vgd::Descriptor::Kind::rtv)
            views[i] = d->rtv;
    }
    const vgd::Descriptor* ds = dsv && dsv->ptr ? descriptor_at(*dsv) : nullptr;
    list_of(hl).set_render_targets({views.data(), count},
                                   ds && ds->kind == vgd::Descriptor::Kind::dsv && ds->dsv.view ? &ds->dsv : nullptr);
}

void APIENTRY clear_depth_stencil_view(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_CPU_DESCRIPTOR_HANDLE h, UINT flags,
                                       FLOAT depth, UINT8 stencil, UINT count, const D3D12DDI_RECT* rects)
{
    const vgd::Descriptor* d = descriptor_at(h);
    if (d->kind != vgd::Descriptor::Kind::dsv || !d->dsv.view) {
        object<CommandListObject>(hl)->error(E_INVALIDARG);
        return;
    }
    list_of(hl).clear_depth_stencil_view(d->dsv, static_cast<D3D12_CLEAR_FLAGS>(flags), depth, stencil,
                                         {rects, rects ? count : 0});
}

void APIENTRY om_set_depth_bounds(D3D12DDI_HCOMMANDLIST hl, FLOAT min, FLOAT max)
{
    list_of(hl).set_depth_bounds(min, max);
}

void APIENTRY resource_resolve_subresource(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HRESOURCE dst, UINT dst_sub,
                                           D3D12DDI_HRESOURCE src, UINT src_sub, DXGI_FORMAT format)
{
    auto* d = object<ResourceObject>(dst);
    auto* s = object<ResourceObject>(src);
    if (!d || !s || !d->resource || !s->resource) {
        object<CommandListObject>(hl)->error(E_INVALIDARG);
        return;
    }
    list_of(hl).resolve_subresource(*d->resource, dst_sub, *s->resource, src_sub, format);
}

void APIENTRY clear_render_target_view(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_CPU_DESCRIPTOR_HANDLE h,
                                       const FLOAT color[4], UINT count, const D3D12DDI_RECT* rects)
{
    const vgd::Descriptor* d = descriptor_at(h);
    if (d->kind != vgd::Descriptor::Kind::rtv || !d->rtv.view) {
        object<CommandListObject>(hl)->error(E_INVALIDARG);
        return;
    }
    list_of(hl).clear_render_target_view(d->rtv, color, {rects, rects ? count : 0});
}

void APIENTRY draw_instanced(D3D12DDI_HCOMMANDLIST hl, UINT vertices, UINT instances, UINT start_vertex,
                             UINT start_instance)
{
    list_of(hl).draw_instanced(vertices, instances, start_vertex, start_instance);
}

void APIENTRY draw_indexed_instanced(D3D12DDI_HCOMMANDLIST hl, UINT indices, UINT instances, UINT start_index,
                                     INT base_vertex, UINT start_instance)
{
    list_of(hl).draw_indexed_instanced(indices, instances, start_index, base_vertex, start_instance);
}

// --- Queues --------------------------------------------------------------------------------------------------

SIZE_T APIENTRY calc_private_command_queue_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATECOMMANDQUEUE_0050*)
{
    return sizeof(CommandQueueObject);
}

HRESULT APIENTRY create_command_queue(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATECOMMANDQUEUE_0050* args,
                                      D3D12DDI_HCOMMANDQUEUE hq, D3D12DDI_HRTCOMMANDQUEUE rt)
{
    D3D12_COMMAND_QUEUE_DESC desc{};
    if (!queue_type(args->QueueFlags, &desc.Type)) {
        VGD_WARN("CreateCommandQueue: unsupported queue flags 0x%x", static_cast<unsigned>(args->QueueFlags));
        return E_OUTOFMEMORY;
    }
    Device* d = Device::from(h);
    auto* obj = new (hq.pDrvPrivate) CommandQueueObject;
    if (FAILED(d->core->create_command_queue(desc, &obj->queue))) {
        obj->~CommandQueueObject();
        return E_OUTOFMEMORY;
    }
    obj->bridge = std::make_unique<QueueBridge>(*d, rt, *obj->queue);
    if (FAILED(obj->bridge->init())) {
        obj->~CommandQueueObject();
        return E_OUTOFMEMORY;
    }
    VGD_TRACE("CreateCommandQueue: flags 0x%x", static_cast<unsigned>(args->QueueFlags));
    return S_OK;
}

void APIENTRY destroy_command_queue(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDQUEUE hq)
{
    object<CommandQueueObject>(hq)->~CommandQueueObject();
}

void APIENTRY execute_command_lists(D3D12DDI_HCOMMANDQUEUE hq, UINT count, const D3D12DDI_HCOMMANDLIST* lists)
{
    std::vector<vgd::CommandList*> core_lists(count);
    for (UINT i = 0; i < count; i++)
        core_lists[i] = object<CommandListObject>(lists[i])->list.get();
    vgd::CommandBatch batch;
    HRESULT hr = vgd::CommandQueue::capture(core_lists, &batch);
    VGD_VERBOSE("ExecuteCommandLists: %u lists", count);
    if (SUCCEEDED(hr))
        hr = object<CommandQueueObject>(hq)->bridge->execute(std::move(batch));
    if (FAILED(hr))
        VGD_WARN("ExecuteCommandLists(%u) failed: 0x%08lx", count, hr);
}

} // namespace

void fill_command_ddi(D3D12DDI_DEVICE_FUNCS_CORE_0088& t)
{
    t.pfnCalcPrivateCommandPoolSize = calc_private_command_pool_size;
    t.pfnCreateCommandPool = create_command_pool;
    t.pfnDestroyCommandPool = destroy_command_pool;
    t.pfnResetCommandPool = reset_command_pool;
    t.pfnCalcPrivateCommandRecorderSize = calc_private_command_recorder_size;
    t.pfnCreateCommandRecorder = create_command_recorder;
    t.pfnDestroyCommandRecorder = destroy_command_recorder;
    t.pfnCommandRecorderSetCommandPoolAsTarget = set_command_pool_as_target;
    t.pfnCalcPrivateCommandListSize = calc_private_command_list_size;
    t.pfnCreateCommandList = create_command_list;
    t.pfnDestroyCommandList = destroy_command_list;
    t.pfnCalcPrivateCommandQueueSize = calc_private_command_queue_size;
    t.pfnCreateCommandQueue = create_command_queue;
    t.pfnDestroyCommandQueue = destroy_command_queue;
}

void fill_command_list_ddi(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& t)
{
    t.pfnCloseCommandList = close_command_list;
    t.pfnResetCommandList = reset_command_list;
    t.pfnCopyBufferRegion = copy_buffer_region;
    t.pfnResourceCopy = resource_copy;
    t.pfnResourceBarrier = resource_barrier;
    t.pfnCopyTextureRegion = copy_texture_region;
    t.pfnSetPipelineState = set_pipeline_state;
    t.pfnSetGraphicsRootSignature = set_graphics_root_signature;
    t.pfnSetGraphicsRoot32BitConstant = set_graphics_root_32bit_constant;
    t.pfnSetGraphicsRoot32BitConstants = set_graphics_root_32bit_constants;
    t.pfnSetGraphicsRootConstantBufferView = set_graphics_root_buffer_view;
    t.pfnSetGraphicsRootShaderResourceView = set_graphics_root_buffer_view;
    t.pfnSetGraphicsRootUnorderedAccessView = set_graphics_root_buffer_view;
    t.pfnClearRootArguments = clear_root_arguments;
    t.pfnIaSetTopology = ia_set_topology;
    t.pfnRsSetViewports = rs_set_viewports;
    t.pfnRsSetScissorRects = rs_set_scissor_rects;
    t.pfnOmSetBlendFactor = om_set_blend_factor;
    t.pfnOmSetStencilRef = om_set_stencil_ref;
    t.pfnIASetVertexBuffers = ia_set_vertex_buffers;
    t.pfnIASetIndexBuffer = ia_set_index_buffer;
    t.pfnOMSetRenderTargets = om_set_render_targets;
    t.pfnClearRenderTargetView = clear_render_target_view;
    t.pfnClearDepthStencilView = clear_depth_stencil_view;
    t.pfnOMSetDepthBounds = om_set_depth_bounds;
    t.pfnResourceResolveSubresource = resource_resolve_subresource;
    t.pfnDrawInstanced = draw_instanced;
    t.pfnDrawIndexedInstanced = draw_indexed_instanced;
    // Default state the runtime sets on every reset, with nothing to record yet: no stream output or predication.
    t.pfnSetDescriptorHeaps = set_descriptor_heaps;
    t.pfnSetGraphicsRootDescriptorTable = set_graphics_root_descriptor_table;
    set_noop(t.pfnSOSetTargets);
    set_noop(t.pfnSetPredication);
}

void fill_command_queue_ddi(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001& t)
{
    t.pfnExecuteCommandLists = execute_command_lists;
}

} // namespace vgd::umd
