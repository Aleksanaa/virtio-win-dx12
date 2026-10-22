// SPDX-License-Identifier: MIT
// Heaps, resources, CPU mapping, fences and tiled resources, on vkd3d-proton's. Memory belongs to vkd3d-proton (no
// WDDM allocations; residency is a no-op), except for swapchain buffers and opened shared textures (present.cpp).
#include "device.h"
#include "log.h"
#include "objects.h"

#include <algorithm>
#include <atomic>
#include <new>

namespace vgd::umd {

D3D12_RESOURCE_DESC1 resource_desc(const D3D12DDIARG_CREATERESOURCE_0088& r)
{
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = static_cast<D3D12_RESOURCE_DIMENSION>(r.ResourceType);
    d.Width = r.Width;
    d.Height = r.Height;
    d.DepthOrArraySize = r.DepthOrArraySize;
    d.MipLevels = r.MipLevels;
    d.Format = r.Format;
    d.SampleDesc = r.SampleDesc;
    d.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(r.Layout);
    // The DDI says what a resource may be used as, where the API denies shader resource use.
    if (r.Flags & D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET)
        d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (r.Flags & D3D12DDI_RESOURCE_FLAG_0003_DEPTH_STENCIL)
        d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    if (r.Flags & D3D12DDI_RESOURCE_FLAG_0022_UNORDERED_ACCESS)
        d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (r.Flags & D3D12DDI_RESOURCE_FLAG_0003_SIMULTANEOUS_ACCESS)
        d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    if (r.Flags & D3D12DDI_RESOURCE_FLAG_0003_CROSS_ADAPTER)
        d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    if (r.Flags & D3D12DDI_RESOURCE_FLAG_0088_RAYTRACING_ACCELERATION_STRUCTURE)
        d.Flags |= D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE;
    if (r.ResourceType != D3D12DDI_RT_BUFFER && !(r.Flags & D3D12DDI_RESOURCE_FLAG_0003_SHADER_RESOURCE))
        d.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    return d;
}

namespace {

// The runtime has already turned the API heap type into a memory pool and CPU page property for our caps (UMA, not
// cache coherent): DEFAULT has no CPU access, UPLOAD is write-combined and READBACK is write-back.
D3D12_HEAP_PROPERTIES heap_properties(const D3D12DDIARG_CREATEHEAP_0001& h)
{
    D3D12_HEAP_PROPERTIES p{};
    switch (h.CPUPageProperty) {
    case D3D12DDI_CPU_PAGE_PROPERTY_WRITE_COMBINE: p.Type = D3D12_HEAP_TYPE_UPLOAD; break;
    case D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK: p.Type = D3D12_HEAP_TYPE_READBACK; break;
    default: p.Type = D3D12_HEAP_TYPE_DEFAULT; break;
    }
    return p;
}

// The API's heap flags of a DDI heap: the categories it allows (none named: no restriction) as denials.
D3D12_HEAP_FLAGS heap_flags(const D3D12DDIARG_CREATEHEAP_0001& h)
{
    constexpr auto kCategories =
        D3D12DDI_HEAP_FLAG_BUFFERS | D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES | D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES;
    D3D12_HEAP_FLAGS f = D3D12_HEAP_FLAG_NONE;
    if (h.Flags & kCategories) {
        if (!(h.Flags & D3D12DDI_HEAP_FLAG_BUFFERS))
            f |= D3D12_HEAP_FLAG_DENY_BUFFERS;
        if (!(h.Flags & D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES))
            f |= D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES;
        if (!(h.Flags & D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES))
            f |= D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES;
    }
    return f;
}

// The DDI tracks no legacy initial state: the states the API requires of resources on each heap type.
D3D12_RESOURCE_STATES initial_state(D3D12_HEAP_TYPE type)
{
    return type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
           : type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
                                              : D3D12_RESOURCE_STATE_COMMON;
}

void log_failure(const char* what, const D3D12_RESOURCE_DESC1& desc, HRESULT hr)
{
    VGD_WARN("CreateHeapAndResource: %s dimension %d, %llux%ux%u, mips %u, format %d, flags 0x%x failed: 0x%08lx", what,
             static_cast<int>(desc.Dimension), static_cast<unsigned long long>(desc.Width), desc.Height,
             desc.DepthOrArraySize, desc.MipLevels, static_cast<int>(desc.Format), static_cast<unsigned>(desc.Flags), hr);
}

// --- Heaps and resources -------------------------------------------------------------------------------------

D3D12DDI_HEAP_AND_RESOURCE_SIZES APIENTRY calc_private_heap_and_resource_sizes(
    D3D12DDI_HDEVICE, const D3D12DDIARG_CREATEHEAP_0001* heap, const D3D12DDIARG_CREATERESOURCE_0088*,
    D3D12DDI_HPROTECTEDRESOURCESESSION_0030)
{
    // A heap alone has a resource handle too (placed resources name their heap by it).
    return {heap ? sizeof(HeapObject) : 0, sizeof(ResourceObject)};
}

HRESULT create_heap(Device& d, const D3D12DDIARG_CREATEHEAP_0001& h, uint64_t min_size, HeapObject& out)
{
    D3D12_HEAP_DESC desc{};
    desc.SizeInBytes = h.ByteSize == UINT64_MAX ? min_size : std::max(h.ByteSize, min_size);
    desc.Properties = heap_properties(h);
    desc.Alignment = h.Alignment;
    desc.Flags = heap_flags(h);
    HRESULT hr = d.d3d->CreateHeap(&desc, __uuidof(ID3D12Heap), out.heap.put_void());
    if (FAILED(hr))
        VGD_WARN("CreateHeap: %llu bytes, alignment %llu, type %d, flags 0x%x failed: 0x%08lx",
                 static_cast<unsigned long long>(desc.SizeInBytes), static_cast<unsigned long long>(desc.Alignment),
                 static_cast<int>(desc.Properties.Type), static_cast<unsigned>(desc.Flags), hr);
    return hr;
}

HRESULT create_placed(Device& d, ID3D12Heap* heap, uint64_t offset, const D3D12DDIARG_CREATERESOURCE_0088& r,
                      const D3D12_RESOURCE_DESC1& desc, Com<ID3D12Resource>* out)
{
    const D3D12_HEAP_DESC hd = desc_of(heap);
    const D3D12_RESOURCE_STATES state = initial_state(hd.Properties.Type);
    if (r.NumCastableFormats) {
        if (auto d10 = d.engine->device.as<ID3D12Device10>())
            return d10->CreatePlacedResource2(heap, offset, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr,
                                              r.NumCastableFormats, r.pCastableFormats, __uuidof(ID3D12Resource),
                                              out->put_void());
    }
    return d.d3d8->CreatePlacedResource1(heap, offset, &desc, state, nullptr, __uuidof(ID3D12Resource),
                                         out->put_void());
}

// A placed resource (CreatePlacedResource) or, naming no heap, a reserved (tiled) one.
HRESULT create_unowned(Device& d, const D3D12DDIARG_CREATERESOURCE_0088& r, D3D12DDI_HRESOURCE hr_)
{
    const D3D12DDIARG_HRESOURCE_PLACEMENT& p = r.ReuseBufferGPUVA.BaseAddress.UMD;
    const D3D12_RESOURCE_DESC1 desc = resource_desc(r);
    auto* ro = new (hr_.pDrvPrivate) ResourceObject;
    HRESULT hr;
    if (!p.hResource.pDrvPrivate) {
        D3D12_RESOURCE_DESC d0{desc.Dimension, desc.Alignment, desc.Width, desc.Height, desc.DepthOrArraySize,
                               desc.MipLevels, desc.Format, desc.SampleDesc, desc.Layout, desc.Flags};
        hr = d.d3d->CreateReservedResource(&d0, D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource),
                                           ro->placed.put_void());
        if (FAILED(hr))
            log_failure("reserved", desc, hr);
    } else {
        const ResourceObject* base = object<ResourceObject>(p.hResource);
        HeapObject* heap = base->heap ? base->heap : base->placed_in;
        const uint64_t offset = base->offset + p.Offset;
        hr = heap && heap->heap ? create_placed(d, heap->heap.get(), offset, r, desc, &ro->placed) : E_INVALIDARG;
        if (FAILED(hr))
            log_failure("placed", desc, hr);
        ro->placed_in = heap;
        ro->offset = offset;
    }
    if (FAILED(hr)) {
        ro->~ResourceObject();
        return E_OUTOFMEMORY;
    }
    ro->resource = ro->placed.get();
    return S_OK;
}

HRESULT APIENTRY create_heap_and_resource(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATEHEAP_0001* heap,
                                          D3D12DDI_HHEAP hh, D3D12DDI_HRTRESOURCE rt_resource,
                                          const D3D12DDIARG_CREATERESOURCE_0088* resource,
                                          const D3D12DDI_CLEAR_VALUES*, D3D12DDI_HPROTECTEDRESOURCESESSION_0030,
                                          D3D12DDI_HRESOURCE hr_)
{
    Device& d = *Device::from(h);
    if (!heap && resource)
        return create_unowned(d, *resource, hr_);
    if (!heap) {
        VGD_WARN("CreateHeapAndResource: neither a heap nor a resource description");
        return E_OUTOFMEMORY;
    }
    auto* ho = new (hh.pDrvPrivate) HeapObject;
    auto fail = [&](HRESULT hr) {
        ho->~HeapObject();
        return FAILED(hr) ? E_OUTOFMEMORY : hr;
    };
    if (!resource) {
        HRESULT hr = create_heap(d, *heap, 0, *ho);
        if (FAILED(hr))
            return fail(hr);
        if (hr_.pDrvPrivate)
            new (hr_.pDrvPrivate) ResourceObject{ho, nullptr};
        return S_OK;
    }
    const D3D12_RESOURCE_DESC1 desc = resource_desc(*resource);
    if (heap->Flags & D3D12DDI_HEAP_FLAG_PRIMARY) {
        // A swapchain buffer: the compositor reads it through a runtime allocation.
        const D3D12_RESOURCE_DESC d0{desc.Dimension, desc.Alignment, desc.Width, desc.Height, desc.DepthOrArraySize,
                                     desc.MipLevels, desc.Format, desc.SampleDesc, desc.Layout, desc.Flags};
        HRESULT hr = create_primary(d, *heap, *resource, d0, rt_resource, *ho);
        if (FAILED(hr)) {
            ho->~HeapObject();
            return hr;
        }
        new (hr_.pDrvPrivate) ResourceObject{ho, ho->owned()};
        return S_OK;
    }
    HRESULT hr;
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        // A heap with the buffer at 0, so placed resources can name the heap by the buffer (see objects.h).
        const D3D12_RESOURCE_ALLOCATION_INFO info = allocation_info(d.d3d8.get(), desc);
        hr = create_heap(d, *heap, info.SizeInBytes, *ho);
        if (SUCCEEDED(hr))
            hr = create_placed(d, ho->heap.get(), 0, *resource, desc, &ho->resource);
    } else {
        const D3D12_HEAP_PROPERTIES props = heap_properties(*heap);
        auto d10 = resource->NumCastableFormats ? d.engine->device.as<ID3D12Device10>() : Com<ID3D12Device10>();
        if (d10)
            hr = d10->CreateCommittedResource3(&props, heap_flags(*heap), &desc, D3D12_BARRIER_LAYOUT_UNDEFINED,
                                               nullptr, nullptr, resource->NumCastableFormats,
                                               resource->pCastableFormats, __uuidof(ID3D12Resource),
                                               ho->resource.put_void());
        else
            hr = d.d3d8->CreateCommittedResource2(&props, heap_flags(*heap), &desc, initial_state(props.Type), nullptr,
                                                  nullptr, __uuidof(ID3D12Resource), ho->resource.put_void());
    }
    if (FAILED(hr)) {
        log_failure("committed", desc, hr);
        return fail(hr);
    }
    new (hr_.pDrvPrivate) ResourceObject{ho, ho->owned()};
    return S_OK;
}

void APIENTRY destroy_heap_and_resource(D3D12DDI_HDEVICE h, D3D12DDI_HHEAP hh, D3D12DDI_HRESOURCE hr_)
{
    if (hh.pDrvPrivate && object<HeapObject>(hh)->allocation)
        destroy_primary(*Device::from(h), *object<HeapObject>(hh));
    if (hr_.pDrvPrivate)
        object<ResourceObject>(hr_)->~ResourceObject();
    if (hh.pDrvPrivate)
        object<HeapObject>(hh)->~HeapObject();
}

// Placed resources are mapped through their heap, the runtime adding their offset: the heap's mapping is that of the
// buffer spanning it.
HRESULT APIENTRY map_heap(D3D12DDI_HDEVICE h, D3D12DDI_HHEAP hh, void** data)
{
    auto* ho = object<HeapObject>(hh);
    if (!ho->mapped) {
        ID3D12Resource* r = ho->resource.get();
        if (!r && ho->heap) {
            const D3D12_HEAP_DESC hd = desc_of(ho->heap.get());
            const D3D12_RESOURCE_DESC1 buffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, hd.SizeInBytes, 1, 1, 1,
                                              DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
            Device& d = *Device::from(h);
            if (SUCCEEDED(d.d3d8->CreatePlacedResource1(ho->heap.get(), 0, &buffer, initial_state(hd.Properties.Type),
                                                        nullptr, __uuidof(ID3D12Resource),
                                                        ho->map_buffer.put_void())))
                r = ho->map_buffer.get();
        }
        if (!r || FAILED(r->Map(0, nullptr, &ho->mapped)))
            ho->mapped = nullptr;
    }
    if (!ho->mapped) {
        VGD_WARN("MapHeap: heap has no CPU mapping");
        return E_INVALIDARG;
    }
    *data = ho->mapped;
    return S_OK;
}

void APIENTRY unmap_heap(D3D12DDI_HDEVICE, D3D12DDI_HHEAP)
{
    // Mappings are persistent and the memory is host coherent.
}

D3D12DDI_GPU_VIRTUAL_ADDRESS APIENTRY check_resource_virtual_address(D3D12DDI_HDEVICE, D3D12DDI_HRESOURCE hr_)
{
    ID3D12Resource* r = resource_of(hr_);
    return r && desc_of(r).Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? r->GetGPUVirtualAddress() : 0;
}

// We keep no additional data next to a resource. Its alignments must still be powers of two: the runtime aligns its
// GetResourceAllocationInfo answer up to them, and zero wraps it to 0xFFFFFFFFFFFF0000 (measured in the guest).
void fill_allocation_info(const D3D12_RESOURCE_ALLOCATION_INFO& a, D3D12_RESOURCE_DIMENSION dim,
                          D3D12DDI_RESOURCE_ALLOCATION_INFO_0022* info)
{
    *info = {};
    const bool ok = a.SizeInBytes != UINT64_MAX;
    info->ResourceDataSize = ok ? a.SizeInBytes : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    info->ResourceDataAlignment = ok ? static_cast<UINT32>(a.Alignment) : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    info->AdditionalDataHeaderAlignment = info->ResourceDataAlignment;
    info->AdditionalDataAlignment = info->ResourceDataAlignment;
    info->Layout = dim == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12DDI_TL_ROW_MAJOR : D3D12DDI_TL_UNDEFINED;
}

void APIENTRY check_resource_allocation_info(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATERESOURCE_0088* resource,
                                             D3D12DDI_RESOURCE_OPTIMIZATION_FLAGS, UINT32, UINT,
                                             D3D12DDI_RESOURCE_ALLOCATION_INFO_0022* info)
{
    const D3D12_RESOURCE_DESC1 desc = resource_desc(*resource);
    D3D12_RESOURCE_ALLOCATION_INFO a = allocation_info(Device::from(h)->d3d8.get(), desc);
    if (a.SizeInBytes == UINT64_MAX)
        VGD_WARN("CheckResourceAllocationInfo: dimension %d, format %d not supported", static_cast<int>(desc.Dimension),
                 static_cast<int>(desc.Format));
    fill_allocation_info(a, desc.Dimension, info);
}

void APIENTRY check_existing_resource_allocation_info(D3D12DDI_HDEVICE h, D3D12DDI_HRESOURCE hr_,
                                                      D3D12DDI_RESOURCE_ALLOCATION_INFO_0022* info)
{
    ID3D12Resource* r = resource_of(hr_);
    if (!r) {
        fill_allocation_info({UINT64_MAX, 0}, D3D12_RESOURCE_DIMENSION_BUFFER, info);
        return;
    }
    const D3D12_RESOURCE_DESC desc = desc_of(r);
    D3D12_RESOURCE_ALLOCATION_INFO a;
    Device::from(h)->d3d->GetResourceAllocationInfo(&a, 0, 1, &desc);
    fill_allocation_info(a, desc.Dimension, info);
}

// Used by the runtime's Map to find a subresource within the heap mapping. A buffer is one subresource at 0.
void APIENTRY check_subresource_info(D3D12DDI_HDEVICE, D3D12DDI_HRESOURCE hr_, UINT subresource,
                                     D3D12DDI_SUBRESOURCE_INFO* info)
{
    ID3D12Resource* r = resource_of(hr_);
    *info = {};
    const D3D12_RESOURCE_DESC desc = r ? desc_of(r) : D3D12_RESOURCE_DESC{};
    info->RowStride = desc.Width;
    info->DepthStride = desc.Width;
    if (subresource != 0 || desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
        VGD_WARN("CheckSubresourceInfo: subresource %u of a texture (textures have no CPU mapping)", subresource);
}

// --- Fences --------------------------------------------------------------------------------------------------

SIZE_T APIENTRY calc_private_fence_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_FENCE*)
{
    return sizeof(FenceObject);
}

HRESULT APIENTRY create_fence(D3D12DDI_HDEVICE, D3D12DDI_HFENCE hf, const D3D12DDIARG_CREATE_FENCE* args)
{
    if (!args || args->FenceCount != 1 || !args->Fences) {
        VGD_WARN("CreateFence: %u fences (only single-adapter fences are supported)", args ? args->FenceCount : 0);
        return E_OUTOFMEMORY;
    }
    new (hf.pDrvPrivate) FenceObject{args->Fences[0]};
    return S_OK;
}

void APIENTRY destroy_fence(D3D12DDI_HDEVICE, D3D12DDI_HFENCE hf)
{
    object<FenceObject>(hf)->~FenceObject();
}

// --- Opened shared resources ---------------------------------------------------------------------------------
// Textures other drivers share (present.cpp open_shared): the heap owns the imported texture, as a committed one.

D3D12DDI_HEAP_AND_RESOURCE_SIZES APIENTRY calc_private_opened_heap_and_resource_sizes(
    D3D12DDI_HDEVICE, const D3D12DDIARG_OPENHEAP_0003*, D3D12DDI_HPROTECTEDRESOURCESESSION_0030)
{
    return {sizeof(HeapObject), sizeof(ResourceObject)};
}

HRESULT APIENTRY open_heap_and_resource(D3D12DDI_HDEVICE h, const D3D12DDIARG_OPENHEAP_0003* args,
                                        D3D12DDI_HHEAP hh, D3D12DDI_HRTRESOURCE,
                                        D3D12DDI_HPROTECTEDRESOURCESESSION_0030, D3D12DDI_HRESOURCE hr_)
{
    if (!args) {
        VGD_WARN("OpenHeapAndResource: no arguments");
        return E_OUTOFMEMORY;
    }
    auto* ho = new (hh.pDrvPrivate) HeapObject;
    HRESULT hr = open_shared(*Device::from(h), *args, *ho);
    if (FAILED(hr)) {
        ho->~HeapObject();
        return hr;
    }
    new (hr_.pDrvPrivate) ResourceObject{ho, ho->owned()};
    return S_OK;
}

// --- Tiled resources -----------------------------------------------------------------------------------------
// The DDI's tile structs are the API's field for field.
static_assert(sizeof(D3D12DDI_TILED_RESOURCE_COORDINATE) == sizeof(D3D12_TILED_RESOURCE_COORDINATE));
static_assert(sizeof(D3D12DDI_TILE_REGION_SIZE) == sizeof(D3D12_TILE_REGION_SIZE));

void APIENTRY get_mip_packing(D3D12DDI_HDEVICE h, D3D12DDI_HRESOURCE hr_, UINT* packed_mips, UINT* packed_tiles)
{
    D3D12_PACKED_MIP_INFO info{};
    if (ID3D12Resource* r = resource_of(hr_))
        Device::from(h)->d3d->GetResourceTiling(r, nullptr, &info, nullptr, nullptr, 0, nullptr);
    *packed_mips = info.NumPackedMips;
    *packed_tiles = info.NumTilesForPackedMips;
}

ID3D12CommandQueue* queue_of(D3D12DDI_HCOMMANDQUEUE hq)
{
    return object<CommandQueueObject>(hq)->queue.get();
}

void APIENTRY update_tile_mappings(D3D12DDI_HCOMMANDQUEUE hq, D3D12DDI_HRESOURCE hr_, UINT regions,
                                   const D3D12DDI_TILED_RESOURCE_COORDINATE* coords,
                                   const D3D12DDI_TILE_REGION_SIZE* sizes, D3D12DDI_HHEAP hh, UINT ranges,
                                   const D3D12DDI_TILE_RANGE_FLAGS* flags, const UINT* heap_offsets,
                                   const UINT* range_counts, D3D12DDI_TILE_MAPPING_FLAGS mapping_flags)
{
    ID3D12Heap* heap = hh.pDrvPrivate ? object<HeapObject>(hh)->heap.get() : nullptr;
    queue_of(hq)->UpdateTileMappings(resource_of(hr_), regions,
                                     reinterpret_cast<const D3D12_TILED_RESOURCE_COORDINATE*>(coords),
                                     reinterpret_cast<const D3D12_TILE_REGION_SIZE*>(sizes), heap, ranges,
                                     reinterpret_cast<const D3D12_TILE_RANGE_FLAGS*>(flags), heap_offsets,
                                     range_counts, static_cast<D3D12_TILE_MAPPING_FLAGS>(mapping_flags));
}

void APIENTRY copy_tile_mappings(D3D12DDI_HCOMMANDQUEUE hq, D3D12DDI_HRESOURCE dst,
                                 const D3D12DDI_TILED_RESOURCE_COORDINATE* dst_coord, D3D12DDI_HRESOURCE src,
                                 const D3D12DDI_TILED_RESOURCE_COORDINATE* src_coord,
                                 const D3D12DDI_TILE_REGION_SIZE* size, D3D12DDI_TILE_MAPPING_FLAGS flags)
{
    queue_of(hq)->CopyTileMappings(resource_of(dst), reinterpret_cast<const D3D12_TILED_RESOURCE_COORDINATE*>(dst_coord),
                                   resource_of(src), reinterpret_cast<const D3D12_TILED_RESOURCE_COORDINATE*>(src_coord),
                                   reinterpret_cast<const D3D12_TILE_REGION_SIZE*>(size),
                                   static_cast<D3D12_TILE_MAPPING_FLAGS>(flags));
}

} // namespace

void fill_resource_ddi(D3D12DDI_DEVICE_FUNCS_CORE_0088& t)
{
    t.pfnCalcPrivateHeapAndResourceSizes = calc_private_heap_and_resource_sizes;
    t.pfnCreateHeapAndResource = create_heap_and_resource;
    t.pfnDestroyHeapAndResource = destroy_heap_and_resource;
    t.pfnCalcPrivateOpenedHeapAndResourceSizes = calc_private_opened_heap_and_resource_sizes;
    t.pfnOpenHeapAndResource = open_heap_and_resource;
    t.pfnMapHeap = map_heap;
    t.pfnUnmapHeap = unmap_heap;
    t.pfnCheckResourceVirtualAddress = check_resource_virtual_address;
    t.pfnCheckResourceAllocationInfo = check_resource_allocation_info;
    t.pfnCheckExistingResourceAllocationInfo = check_existing_resource_allocation_info;
    t.pfnCheckSubresourceInfo = check_subresource_info;
    t.pfnCalcPrivateFenceSize = calc_private_fence_size;
    t.pfnCreateFence = create_fence;
    t.pfnDestroyFence = destroy_fence;
    t.pfnGetMipPacking = get_mip_packing;
}

void fill_resource_queue_ddi(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001& t)
{
    t.pfnUpdateTileMappings = update_tile_mappings;
    t.pfnCopyTileMappings = copy_tile_mappings;
}

} // namespace vgd::umd
