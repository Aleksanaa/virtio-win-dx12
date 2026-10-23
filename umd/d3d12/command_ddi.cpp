// SPDX-License-Identifier: MIT
// Command pools, recorders, lists and queues (the DDI model since 0040), on vkd3d-proton's allocators, lists and
// queues:
//  - a pool is untyped; it owns one command allocator per list type, created when first needed;
//  - a recorder (the API's allocator, as the runtime sees it) has a queue type and a target pool;
//  - ResetCommandList names a recorder; the list then records into the target pool's allocator for its type.
// Recording commands are the API's, one for one; the DDI only names objects by handle.
#include "device.h"
#include "log.h"
#include "objects.h"
#include "queue_bridge.h"
#include "stubs.h"

#include <algorithm>
#include <array>
#include <new>
#include <vector>

namespace vgd::umd {

void CommandListObject::error(HRESULT hr) const
{
    VGD_WARN("command list error 0x%08lx", hr);
    device->um_callbacks->pfnSetCommandListErrorCb(rt, hr);
}

namespace {

constexpr size_t kListTypes = 4;   // direct, compute, copy, bundle

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
    switch (type) {
    case D3D12_COMMAND_LIST_TYPE_COMPUTE: return 1;
    case D3D12_COMMAND_LIST_TYPE_COPY: return 2;
    case D3D12_COMMAND_LIST_TYPE_BUNDLE: return 3;
    default: return 0;
    }
}

struct CommandPoolObject {
    std::array<Com<ID3D12CommandAllocator>, kListTypes> allocators;
};

struct CommandRecorderObject {
    CommandPoolObject* pool = nullptr;
};

// --- Pools ---------------------------------------------------------------------------------------------------

SIZE_T APIENTRY calc_private_command_pool_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_COMMAND_POOL_0040*)
{
    return sizeof(CommandPoolObject);
}

HRESULT APIENTRY create_command_pool(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_COMMAND_POOL_0040*,
                                     D3D12DDI_HCOMMANDPOOL_0040 hp)
{
    new (hp.pDrvPrivate) CommandPoolObject;
    return S_OK;
}

void APIENTRY destroy_command_pool(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDPOOL_0040 hp)
{
    object<CommandPoolObject>(hp)->~CommandPoolObject();
}

void APIENTRY reset_command_pool(D3D12DDI_HDEVICE h, D3D12DDI_HCOMMANDPOOL_0040 hp)
{
    for (auto& a : object<CommandPoolObject>(hp)->allocators)
        if (a && FAILED(a->Reset()))
            Device::from(h)->set_error(E_OUTOFMEMORY);
}

// --- Recorders -----------------------------------------------------------------------------------------------

SIZE_T APIENTRY calc_private_command_recorder_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_COMMAND_RECORDER_0040*)
{
    return sizeof(CommandRecorderObject);
}

HRESULT APIENTRY create_command_recorder(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_COMMAND_RECORDER_0040*,
                                         D3D12DDI_HCOMMANDRECORDER_0040 hr_)
{
    new (hr_.pDrvPrivate) CommandRecorderObject;
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
    D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_BUNDLE;
    if (args->Type != D3D12DDI_COMMAND_LIST_TYPE_BUNDLE && !queue_type(args->QueueFlags, &type)) {
        VGD_WARN("CreateCommandList: unsupported queue flags 0x%x", static_cast<unsigned>(args->QueueFlags));
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
    auto* obj = new (hl.pDrvPrivate) CommandListObject{d, rt, type};
    // Created closed: the runtime resets it before recording.
    HRESULT hr = d->d3d4 ? d->d3d4->CreateCommandList1(args->NodeMask, type, D3D12_COMMAND_LIST_FLAG_NONE,
                                                       __uuidof(ID3D12GraphicsCommandList), obj->list.put_void())
                         : E_NOINTERFACE;
    if (FAILED(hr)) {
        VGD_WARN("CreateCommandList: type %d failed: 0x%08lx", static_cast<int>(type), hr);
        obj->~CommandListObject();
        return E_OUTOFMEMORY;
    }
    obj->list1 = obj->list.as<ID3D12GraphicsCommandList1>();
    obj->list2 = obj->list.as<ID3D12GraphicsCommandList2>();
    obj->list5 = obj->list.as<ID3D12GraphicsCommandList5>();
    return S_OK;
}

void APIENTRY destroy_command_list(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDLIST hl)
{
    object<CommandListObject>(hl)->~CommandListObject();
}

CommandListObject& obj(D3D12DDI_HCOMMANDLIST hl)
{
    return *object<CommandListObject>(hl);
}

ID3D12GraphicsCommandList* list(D3D12DDI_HCOMMANDLIST hl)
{
    return object<CommandListObject>(hl)->list.get();
}

void APIENTRY close_command_list(D3D12DDI_HCOMMANDLIST hl)
{
    HRESULT hr = list(hl)->Close();
    if (FAILED(hr))
        obj(hl).error(hr);
}

void APIENTRY reset_command_list(D3D12DDI_HCOMMANDLIST hl, const D3D12DDIARG_RESETCOMMANDLIST_0040* args)
{
    CommandListObject& l = obj(hl);
    l.present_only = false;
    auto* rec = object<CommandRecorderObject>(args->hDrvCommandRecorder);
    if (!rec->pool) {
        VGD_WARN("ResetCommandList: recorder has no target pool");
        l.error(E_INVALIDARG);
        return;
    }
    auto& alloc = rec->pool->allocators[slot_of(l.type)];
    if (!alloc &&
        FAILED(l.device->d3d->CreateCommandAllocator(l.type, __uuidof(ID3D12CommandAllocator), alloc.put_void()))) {
        l.error(E_OUTOFMEMORY);
        return;
    }
    HRESULT hr = l.list->Reset(alloc.get(), nullptr);
    if (FAILED(hr))
        l.error(hr);
}

// --- Copies, clears, resolves and barriers -------------------------------------------------------------------

ID3D12Resource* placed_resource(const D3D12DDIARG_BUFFER_PLACEMENT& p)
{
    return resource_of(p.BaseAddress.UMD.hResource);
}

void APIENTRY copy_buffer_region(D3D12DDI_HCOMMANDLIST hl, D3D12DDIARG_BUFFER_PLACEMENT dst,
                                 D3D12DDIARG_BUFFER_PLACEMENT src, UINT64 bytes)
{
    list(hl)->CopyBufferRegion(placed_resource(dst), dst.BaseAddress.UMD.Offset, placed_resource(src),
                               src.BaseAddress.UMD.Offset, bytes);
}

void APIENTRY resource_copy(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HRESOURCE dst, D3D12DDI_HRESOURCE src)
{
    list(hl)->CopyResource(resource_of(dst), resource_of(src));
}

// Texel blocks of the formats whose blocks are not single texels: the DDI gives buffer footprints in blocks.
void format_block(DXGI_FORMAT f, UINT* w, UINT* h)
{
    *w = *h = 1;
    if ((f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM) ||
        (f >= DXGI_FORMAT_BC6H_TYPELESS && f <= DXGI_FORMAT_BC7_UNORM_SRGB))
        *w = *h = 4;
    else if (f == DXGI_FORMAT_R8G8_B8G8_UNORM || f == DXGI_FORMAT_G8R8_G8B8_UNORM || f == DXGI_FORMAT_YUY2 ||
             f == DXGI_FORMAT_Y210 || f == DXGI_FORMAT_Y216)
        *w = 2;
}

// One side of CopyTextureRegion. A texture names its subresource in the placement offset; a buffer side is a pitched
// footprint whose dimensions the DDI gives in format blocks.
bool copy_location(const D3D12DDIARG_BUFFER_PLACEMENT& placement, const D3D12DDIARG_PLACED_RESOURCE& layout,
                   D3D12_TEXTURE_COPY_LOCATION* out)
{
    out->pResource = placed_resource(placement);
    switch (layout.Layout) {
    case D3D12DDI_RL_SELECT_SUBRESOURCE:
        out->Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        out->SubresourceIndex = static_cast<UINT>(placement.BaseAddress.UMD.Offset);
        return true;
    case D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED: {
        const auto& p = *static_cast<const D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT*>(layout.pLayout);
        UINT bw, bh;
        format_block(p.Format, &bw, &bh);
        out->Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        out->PlacedFootprint.Offset = placement.BaseAddress.UMD.Offset;
        out->PlacedFootprint.Footprint = {p.Format, p.PhysicalWidth * bw, p.PhysicalHeight * bh, p.PhysicalDepth,
                                          p.Pitch};
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
    static_assert(sizeof(D3D12DDI_BOX) == sizeof(D3D12_BOX));
    D3D12_TEXTURE_COPY_LOCATION d{}, s{};
    if (!copy_location(*dst, dst_layout, &d) || !copy_location(*src, src_layout, &s)) {
        obj(hl).error(E_INVALIDARG);
        return;
    }
    list(hl)->CopyTextureRegion(&d, x, y, z, &s, reinterpret_cast<const D3D12_BOX*>(box));
}

void APIENTRY copy_tiles(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HRESOURCE resource,
                         const D3D12DDI_TILED_RESOURCE_COORDINATE* coord, const D3D12DDI_TILE_REGION_SIZE* size,
                         D3D12DDI_HRESOURCE buffer, UINT64 offset, D3D12DDI_TILE_COPY_FLAGS flags)
{
    list(hl)->CopyTiles(resource_of(resource), reinterpret_cast<const D3D12_TILED_RESOURCE_COORDINATE*>(coord),
                        reinterpret_cast<const D3D12_TILE_REGION_SIZE*>(size), resource_of(buffer), offset,
                        static_cast<D3D12_TILE_COPY_FLAGS>(flags));
}

void APIENTRY resource_barrier(D3D12DDI_HCOMMANDLIST hl, UINT count, const D3D12DDIARG_RESOURCE_BARRIER_0022* b)
{
    std::vector<D3D12_RESOURCE_BARRIER> barriers(count);
    for (UINT i = 0; i < count; i++) {
        D3D12_RESOURCE_BARRIER& o = barriers[i];
        // Begin-only and end-only are the API's; the DDI's atomic-copy and aliasing flags are not.
        o.Flags = static_cast<D3D12_RESOURCE_BARRIER_FLAGS>(
            b[i].Flags & (D3D12DDI_RESOURCE_BARRIER_FLAG_BEGIN_ONLY | D3D12DDI_RESOURCE_BARRIER_FLAG_END_ONLY));
        switch (b[i].Type) {
        case D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION:
            o.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            o.Transition = {resource_of(b[i].Transition.hResource), b[i].Transition.Subresource,
                            static_cast<D3D12_RESOURCE_STATES>(b[i].Transition.StateBefore),
                            static_cast<D3D12_RESOURCE_STATES>(b[i].Transition.StateAfter)};
            break;
        case D3D12DDI_RESOURCE_BARRIER_TYPE_UAV:
            o.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            o.UAV = {resource_of(b[i].UAV.hResource)};
            break;
        default:
            // Ranged barriers replace aliasing barriers: the resource whose range becomes active.
            o.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
            o.Aliasing = {nullptr, resource_of(b[i].Ranged.hResource)};
            break;
        }
    }
    list(hl)->ResourceBarrier(count, barriers.data());
}

void APIENTRY discard_resource(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HRESOURCE hr_,
                               const D3D12DDIARG_DISCARD_RESOURCE_0003* a)
{
    D3D12_DISCARD_REGION region{};
    if (a)
        region = {a->NumRects, reinterpret_cast<const D3D12_RECT*>(a->pRects), a->FirstSubresource,
                  a->NumSubresources};
    list(hl)->DiscardResource(resource_of(hr_), a ? &region : nullptr);
}

void APIENTRY resource_resolve_subresource(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HRESOURCE dst, UINT dst_sub,
                                           D3D12DDI_HRESOURCE src, UINT src_sub, DXGI_FORMAT format)
{
    list(hl)->ResolveSubresource(resource_of(dst), dst_sub, resource_of(src), src_sub, format);
}

void APIENTRY resource_resolve_subresource_region(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HRESOURCE dst, UINT dst_sub,
                                                  UINT x, UINT y, D3D12DDI_HRESOURCE src, UINT src_sub,
                                                  D3D12DDI_RECT* rect, DXGI_FORMAT format, D3D12DDI_RESOLVE_MODE mode)
{
    if (auto* l1 = obj(hl).list1.get())
        l1->ResolveSubresourceRegion(resource_of(dst), dst_sub, x, y, resource_of(src), src_sub, rect, format,
                                     static_cast<D3D12_RESOLVE_MODE>(mode));
}

void APIENTRY clear_render_target_view(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_CPU_DESCRIPTOR_HANDLE h,
                                       const FLOAT color[4], UINT count, const D3D12DDI_RECT* rects)
{
    list(hl)->ClearRenderTargetView({h.ptr}, color, count, rects);
}

void APIENTRY clear_depth_stencil_view(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_CPU_DESCRIPTOR_HANDLE h, UINT flags,
                                       FLOAT depth, UINT8 stencil, UINT count, const D3D12DDI_RECT* rects)
{
    list(hl)->ClearDepthStencilView({h.ptr}, static_cast<D3D12_CLEAR_FLAGS>(flags), depth, stencil, count, rects);
}

void APIENTRY clear_uav_uint(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_GPU_DESCRIPTOR_HANDLE gpu,
                             D3D12DDI_CPU_DESCRIPTOR_HANDLE cpu, D3D12DDI_HRESOURCE hr_, const UINT values[4],
                             UINT count, const D3D12DDI_RECT* rects)
{
    list(hl)->ClearUnorderedAccessViewUint({gpu.ptr}, {cpu.ptr}, resource_of(hr_), values, count, rects);
}

void APIENTRY clear_uav_float(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_GPU_DESCRIPTOR_HANDLE gpu,
                              D3D12DDI_CPU_DESCRIPTOR_HANDLE cpu, D3D12DDI_HRESOURCE hr_, const FLOAT values[4],
                              UINT count, const D3D12DDI_RECT* rects)
{
    list(hl)->ClearUnorderedAccessViewFloat({gpu.ptr}, {cpu.ptr}, resource_of(hr_), values, count, rects);
}

void APIENTRY write_buffer_immediate(D3D12DDI_HCOMMANDLIST hl, UINT count,
                                     const D3D12DDI_WRITEBUFFERIMMEDIATE_PARAMETER_0032* params,
                                     const D3D12DDI_WRITEBUFFERIMMEDIATE_MODE_0032* modes)
{
    static_assert(sizeof(D3D12DDI_WRITEBUFFERIMMEDIATE_PARAMETER_0032) == sizeof(D3D12_WRITEBUFFERIMMEDIATE_PARAMETER));
    if (auto* l2 = obj(hl).list2.get())
        l2->WriteBufferImmediate(count, reinterpret_cast<const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER*>(params),
                                 reinterpret_cast<const D3D12_WRITEBUFFERIMMEDIATE_MODE*>(modes));
}

// --- State ---------------------------------------------------------------------------------------------------
// The DDI's viewport, rect, buffer-view and sample-position structs are the API's field for field.
static_assert(sizeof(D3D12DDI_VIEWPORT) == sizeof(D3D12_VIEWPORT));
static_assert(sizeof(D3D12DDI_VERTEX_BUFFER_VIEW) == sizeof(D3D12_VERTEX_BUFFER_VIEW));
static_assert(sizeof(D3D12DDI_INDEX_BUFFER_VIEW) == sizeof(D3D12_INDEX_BUFFER_VIEW));
static_assert(sizeof(D3D12DDI_STREAM_OUTPUT_BUFFER_VIEW) == sizeof(D3D12_STREAM_OUTPUT_BUFFER_VIEW));
static_assert(sizeof(D3D12DDI_SAMPLE_POSITION) == sizeof(D3D12_SAMPLE_POSITION));

void APIENTRY set_pipeline_state(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HPIPELINESTATE hp)
{
    list(hl)->SetPipelineState(hp.pDrvPrivate ? object<PipelineStateObject>(hp)->pso.get() : nullptr);
}

ID3D12RootSignature* root_of(D3D12DDI_HROOTSIGNATURE h)
{
    return h.pDrvPrivate ? object<RootSignatureObject>(h)->rs.get() : nullptr;
}

void APIENTRY set_graphics_root_signature(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HROOTSIGNATURE h)
{
    list(hl)->SetGraphicsRootSignature(root_of(h));
}

void APIENTRY set_compute_root_signature(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HROOTSIGNATURE h)
{
    list(hl)->SetComputeRootSignature(root_of(h));
}

void APIENTRY set_descriptor_heaps(D3D12DDI_HCOMMANDLIST hl, UINT count, D3D12DDI_HDESCRIPTORHEAP* heaps)
{
    std::array<ID3D12DescriptorHeap*, 2> h{};
    count = std::min<UINT>(count, h.size());
    for (UINT i = 0; i < count; i++)
        h[i] = heaps[i].pDrvPrivate ? object<DescriptorHeapObject>(heaps[i])->heap.get() : nullptr;
    list(hl)->SetDescriptorHeaps(count, h.data());
}

void APIENTRY set_graphics_root_descriptor_table(D3D12DDI_HCOMMANDLIST hl, UINT index, D3D12DDI_GPU_DESCRIPTOR_HANDLE b)
{
    list(hl)->SetGraphicsRootDescriptorTable(index, {b.ptr});
}

void APIENTRY set_compute_root_descriptor_table(D3D12DDI_HCOMMANDLIST hl, UINT index, D3D12DDI_GPU_DESCRIPTOR_HANDLE b)
{
    list(hl)->SetComputeRootDescriptorTable(index, {b.ptr});
}

void APIENTRY set_graphics_root_32bit_constant(D3D12DDI_HCOMMANDLIST hl, UINT index, UINT value, UINT offset)
{
    list(hl)->SetGraphicsRoot32BitConstant(index, value, offset);
}

void APIENTRY set_compute_root_32bit_constant(D3D12DDI_HCOMMANDLIST hl, UINT index, UINT value, UINT offset)
{
    list(hl)->SetComputeRoot32BitConstant(index, value, offset);
}

void APIENTRY set_graphics_root_32bit_constants(D3D12DDI_HCOMMANDLIST hl, UINT index, UINT count, const void* data,
                                                UINT offset)
{
    list(hl)->SetGraphicsRoot32BitConstants(index, count, data, offset);
}

void APIENTRY set_compute_root_32bit_constants(D3D12DDI_HCOMMANDLIST hl, UINT index, UINT count, const void* data,
                                               UINT offset)
{
    list(hl)->SetComputeRoot32BitConstants(index, count, data, offset);
}

void APIENTRY set_graphics_root_cbv(D3D12DDI_HCOMMANDLIST hl, UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS va)
{
    list(hl)->SetGraphicsRootConstantBufferView(index, va);
}

void APIENTRY set_compute_root_cbv(D3D12DDI_HCOMMANDLIST hl, UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS va)
{
    list(hl)->SetComputeRootConstantBufferView(index, va);
}

void APIENTRY set_graphics_root_srv(D3D12DDI_HCOMMANDLIST hl, UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS va)
{
    list(hl)->SetGraphicsRootShaderResourceView(index, va);
}

void APIENTRY set_compute_root_srv(D3D12DDI_HCOMMANDLIST hl, UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS va)
{
    list(hl)->SetComputeRootShaderResourceView(index, va);
}

void APIENTRY set_graphics_root_uav(D3D12DDI_HCOMMANDLIST hl, UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS va)
{
    list(hl)->SetGraphicsRootUnorderedAccessView(index, va);
}

void APIENTRY set_compute_root_uav(D3D12DDI_HCOMMANDLIST hl, UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS va)
{
    list(hl)->SetComputeRootUnorderedAccessView(index, va);
}

void APIENTRY ia_set_topology(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_PRIMITIVE_TOPOLOGY t)
{
    list(hl)->IASetPrimitiveTopology(static_cast<D3D12_PRIMITIVE_TOPOLOGY>(t));
}

void APIENTRY rs_set_viewports(D3D12DDI_HCOMMANDLIST hl, UINT count, const D3D12DDI_VIEWPORT* v)
{
    list(hl)->RSSetViewports(count, reinterpret_cast<const D3D12_VIEWPORT*>(v));
}

void APIENTRY rs_set_scissor_rects(D3D12DDI_HCOMMANDLIST hl, UINT count, const D3D12DDI_RECT* r)
{
    list(hl)->RSSetScissorRects(count, r);
}

void APIENTRY om_set_blend_factor(D3D12DDI_HCOMMANDLIST hl, const FLOAT factor[4])
{
    list(hl)->OMSetBlendFactor(factor);
}

void APIENTRY om_set_stencil_ref(D3D12DDI_HCOMMANDLIST hl, UINT ref)
{
    list(hl)->OMSetStencilRef(ref);
}

void APIENTRY om_set_depth_bounds(D3D12DDI_HCOMMANDLIST hl, FLOAT min, FLOAT max)
{
    if (auto* l1 = obj(hl).list1.get())
        l1->OMSetDepthBounds(min, max);
}

void APIENTRY set_sample_positions(D3D12DDI_HCOMMANDLIST hl, UINT samples, UINT pixels,
                                   D3D12DDI_SAMPLE_POSITION* positions)
{
    if (auto* l1 = obj(hl).list1.get())
        l1->SetSamplePositions(samples, pixels, reinterpret_cast<D3D12_SAMPLE_POSITION*>(positions));
}

void APIENTRY set_view_instance_mask(D3D12DDI_HCOMMANDLIST hl, UINT mask)
{
    if (auto* l1 = obj(hl).list1.get())
        l1->SetViewInstanceMask(mask);
}

void APIENTRY rs_set_shading_rate(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_SHADING_RATE_0062 rate,
                                  const D3D12DDI_SHADING_RATE_COMBINER_0062* combiners)
{
    if (auto* l5 = obj(hl).list5.get())
        l5->RSSetShadingRate(static_cast<D3D12_SHADING_RATE>(rate),
                             reinterpret_cast<const D3D12_SHADING_RATE_COMBINER*>(combiners));
}

void APIENTRY rs_set_shading_rate_image(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HRESOURCE image)
{
    if (auto* l5 = obj(hl).list5.get())
        l5->RSSetShadingRateImage(resource_of(image));
}

void APIENTRY ia_set_vertex_buffers(D3D12DDI_HCOMMANDLIST hl, UINT start, UINT count,
                                    const D3D12DDI_VERTEX_BUFFER_VIEW* views)
{
    list(hl)->IASetVertexBuffers(start, count, reinterpret_cast<const D3D12_VERTEX_BUFFER_VIEW*>(views));
}

void APIENTRY ia_set_index_buffer(D3D12DDI_HCOMMANDLIST hl, const D3D12DDI_INDEX_BUFFER_VIEW* view)
{
    list(hl)->IASetIndexBuffer(reinterpret_cast<const D3D12_INDEX_BUFFER_VIEW*>(view));
}

void APIENTRY so_set_targets(D3D12DDI_HCOMMANDLIST hl, UINT start, UINT count,
                             const D3D12DDI_STREAM_OUTPUT_BUFFER_VIEW* views)
{
    list(hl)->SOSetTargets(start, count, reinterpret_cast<const D3D12_STREAM_OUTPUT_BUFFER_VIEW*>(views));
}

void APIENTRY om_set_render_targets(D3D12DDI_HCOMMANDLIST hl, UINT count, const D3D12DDI_CPU_DESCRIPTOR_HANDLE* rtvs,
                                    BOOL single_range, const D3D12DDI_CPU_DESCRIPTOR_HANDLE* dsv)
{
    list(hl)->OMSetRenderTargets(count, reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(rtvs), single_range,
                                 reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(dsv));
}

void APIENTRY set_predication(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HRESOURCE hr_, UINT64 offset,
                              D3D12DDI_PREDICATION_OP op)
{
    list(hl)->SetPredication(resource_of(hr_), offset, static_cast<D3D12_PREDICATION_OP>(op));
}

// --- Draws, dispatches and bundles ---------------------------------------------------------------------------

void APIENTRY draw_instanced(D3D12DDI_HCOMMANDLIST hl, UINT vertices, UINT instances, UINT start_vertex,
                             UINT start_instance)
{
    list(hl)->DrawInstanced(vertices, instances, start_vertex, start_instance);
}

void APIENTRY draw_indexed_instanced(D3D12DDI_HCOMMANDLIST hl, UINT indices, UINT instances, UINT start_index,
                                     INT base_vertex, UINT start_instance)
{
    list(hl)->DrawIndexedInstanced(indices, instances, start_index, base_vertex, start_instance);
}

void APIENTRY dispatch(D3D12DDI_HCOMMANDLIST hl, UINT x, UINT y, UINT z)
{
    list(hl)->Dispatch(x, y, z);
}

void APIENTRY execute_bundle(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HCOMMANDLIST bundle)
{
    list(hl)->ExecuteBundle(list(bundle));
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
    desc.NodeMask = args->NodeMask;
    Device* d = Device::from(h);
    auto* obj = new (hq.pDrvPrivate) CommandQueueObject;
    if (FAILED(d->d3d->CreateCommandQueue(&desc, __uuidof(ID3D12CommandQueue), obj->queue.put_void()))) {
        obj->~CommandQueueObject();
        return E_OUTOFMEMORY;
    }
    obj->bridge = std::make_unique<QueueBridge>(*d, rt, obj->queue.get());
    if (FAILED(obj->bridge->init())) {
        obj->~CommandQueueObject();
        return E_OUTOFMEMORY;
    }
    return S_OK;
}

void APIENTRY destroy_command_queue(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDQUEUE hq)
{
    object<CommandQueueObject>(hq)->~CommandQueueObject();
}

void APIENTRY execute_command_lists(D3D12DDI_HCOMMANDQUEUE hq, UINT count, const D3D12DDI_HCOMMANDLIST* lists)
{
    // The runtime executes its present list before each present even when Present added nothing to it. Executing it
    // would put a wait on our completion on the context just ahead of the present, and a windowed Blt present waits for
    // the context holding the device, which blocks the CPU signal that would complete it: a deadlock. So it is skipped.
    std::vector<ID3D12CommandList*> l;
    l.reserve(count);
    for (UINT i = 0; i < count; i++)
        if (!obj(lists[i]).present_only)
            l.push_back(list(lists[i]));
    if (!l.empty())
        object<CommandQueueObject>(hq)->bridge->execute(static_cast<UINT>(l.size()), l.data());
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
    t.pfnDrawInstanced = draw_instanced;
    t.pfnDrawIndexedInstanced = draw_indexed_instanced;
    t.pfnDispatch = dispatch;
    t.pfnClearUnorderedAccessViewUint = clear_uav_uint;
    t.pfnClearUnorderedAccessViewFloat = clear_uav_float;
    t.pfnClearRenderTargetView = clear_render_target_view;
    t.pfnClearDepthStencilView = clear_depth_stencil_view;
    t.pfnDiscardResource = discard_resource;
    t.pfnCopyTextureRegion = copy_texture_region;
    t.pfnResourceCopy = resource_copy;
    t.pfnCopyTiles = copy_tiles;
    t.pfnCopyBufferRegion = copy_buffer_region;
    t.pfnAtomicCopyBufferRegion = copy_buffer_region;
    t.pfnResourceResolveSubresource = resource_resolve_subresource;
    t.pfnResourceResolveSubresourceRegion = resource_resolve_subresource_region;
    t.pfnExecuteBundle = execute_bundle;
    t.pfnResourceBarrier = resource_barrier;
    t.pfnSetPredication = set_predication;
    t.pfnIaSetTopology = ia_set_topology;
    t.pfnRsSetViewports = rs_set_viewports;
    t.pfnRsSetScissorRects = rs_set_scissor_rects;
    t.pfnOmSetBlendFactor = om_set_blend_factor;
    t.pfnOmSetStencilRef = om_set_stencil_ref;
    t.pfnSetPipelineState = set_pipeline_state;
    t.pfnSetDescriptorHeaps = set_descriptor_heaps;
    t.pfnSetComputeRootSignature = set_compute_root_signature;
    t.pfnSetGraphicsRootSignature = set_graphics_root_signature;
    t.pfnSetComputeRootDescriptorTable = set_compute_root_descriptor_table;
    t.pfnSetGraphicsRootDescriptorTable = set_graphics_root_descriptor_table;
    t.pfnSetComputeRoot32BitConstant = set_compute_root_32bit_constant;
    t.pfnSetGraphicsRoot32BitConstant = set_graphics_root_32bit_constant;
    t.pfnSetComputeRoot32BitConstants = set_compute_root_32bit_constants;
    t.pfnSetGraphicsRoot32BitConstants = set_graphics_root_32bit_constants;
    t.pfnSetComputeRootConstantBufferView = set_compute_root_cbv;
    t.pfnSetGraphicsRootConstantBufferView = set_graphics_root_cbv;
    t.pfnSetComputeRootShaderResourceView = set_compute_root_srv;
    t.pfnSetGraphicsRootShaderResourceView = set_graphics_root_srv;
    t.pfnSetComputeRootUnorderedAccessView = set_compute_root_uav;
    t.pfnSetGraphicsRootUnorderedAccessView = set_graphics_root_uav;
    t.pfnIASetIndexBuffer = ia_set_index_buffer;
    t.pfnIASetVertexBuffers = ia_set_vertex_buffers;
    t.pfnSOSetTargets = so_set_targets;
    t.pfnOMSetRenderTargets = om_set_render_targets;
    t.pfnOMSetDepthBounds = om_set_depth_bounds;
    t.pfnSetSamplePositions = set_sample_positions;
    t.pfnWriteBufferImmediate = write_buffer_immediate;
    t.pfnSetViewInstanceMask = set_view_instance_mask;
    t.pfnRSSetShadingRate = rs_set_shading_rate;
    t.pfnRSSetShadingRateImage = rs_set_shading_rate_image;
    // The runtime calls these on every reset; the API has no counterpart. Markers are PIX's.
    set_noop(t.pfnClearRootArguments);
    set_noop(t.pfnSetMarker);
    set_noop(t.pfnSetProtectedResourceSession);
}

void fill_command_queue_ddi(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001& t)
{
    t.pfnExecuteCommandLists = execute_command_lists;
}

} // namespace vgd::umd
