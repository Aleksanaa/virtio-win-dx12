// SPDX-License-Identifier: MIT
// Heaps, resources, CPU mapping and fences. First version: committed buffers and textures, with memory owned by
// Vulkan (no WDDM allocations; residency is a no-op). Placed resources and heap-only creation come later.
#include "device.h"
#include "log.h"
#include "objects.h"

#include <algorithm>
#include <atomic>
#include <new>

namespace vgd::umd {

namespace {

// The runtime has already turned the API heap type into a memory pool and CPU page property for our caps (UMA, not
// cache coherent): DEFAULT has no CPU access, UPLOAD is write-combined and READBACK is write-back.
D3D12_HEAP_TYPE heap_type(const D3D12DDIARG_CREATEHEAP_0001& h)
{
    switch (h.CPUPageProperty) {
    case D3D12DDI_CPU_PAGE_PROPERTY_WRITE_COMBINE: return D3D12_HEAP_TYPE_UPLOAD;
    case D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK: return D3D12_HEAP_TYPE_READBACK;
    default: return D3D12_HEAP_TYPE_DEFAULT;
    }
}

// The DDI resource description as the API's. Dimensions and layouts share values; the flags differ (the DDI says
// what a resource may be used as, where the API denies shader resource use).
D3D12_RESOURCE_DESC resource_desc(const D3D12DDIARG_CREATERESOURCE_0088& r)
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = static_cast<D3D12_RESOURCE_DIMENSION>(r.ResourceType);
    d.Width = r.Width;
    d.Height = r.Height;
    d.DepthOrArraySize = r.DepthOrArraySize;
    d.MipLevels = r.MipLevels;
    d.Format = r.Format;
    d.SampleDesc = r.SampleDesc;
    d.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(r.Layout);
    if (r.Flags & D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET)
        d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (r.Flags & D3D12DDI_RESOURCE_FLAG_0003_DEPTH_STENCIL)
        d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    if (r.Flags & D3D12DDI_RESOURCE_FLAG_0022_UNORDERED_ACCESS)
        d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (r.ResourceType != D3D12DDI_RT_BUFFER && !(r.Flags & D3D12DDI_RESOURCE_FLAG_0003_SHADER_RESOURCE))
        d.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    return d;
}

// --- Heaps and resources -------------------------------------------------------------------------------------

D3D12DDI_HEAP_AND_RESOURCE_SIZES APIENTRY calc_private_heap_and_resource_sizes(
    D3D12DDI_HDEVICE, const D3D12DDIARG_CREATEHEAP_0001* heap, const D3D12DDIARG_CREATERESOURCE_0088*,
    D3D12DDI_HPROTECTEDRESOURCESESSION_0030)
{
    // A heap alone has a resource handle too (placed resources name their heap by it).
    return {heap ? sizeof(HeapObject) : 0, sizeof(ResourceObject)};
}

// The core heap of a heap description: its type, and the categories it allows (none named: no restriction).
// `min_size` covers a committed resource when the description leaves ByteSize to it (UINT64_MAX).
HRESULT create_core_heap(Device& d, const D3D12DDIARG_CREATEHEAP_0001& heap, uint64_t min_size, HeapObject& ho)
{
    const D3D12_HEAP_TYPE type = heap_type(heap);
    constexpr auto kCategories =
        D3D12DDI_HEAP_FLAG_BUFFERS | D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES | D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES;
    D3D12_HEAP_FLAGS flags = D3D12_HEAP_FLAG_NONE;
    if (heap.Flags & kCategories) {
        if (!(heap.Flags & D3D12DDI_HEAP_FLAG_BUFFERS))
            flags |= D3D12_HEAP_FLAG_DENY_BUFFERS;
        if (!(heap.Flags & D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES))
            flags |= D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES;
        if (!(heap.Flags & D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES))
            flags |= D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES;
    }
    const uint64_t size = heap.ByteSize == UINT64_MAX ? min_size : std::max(heap.ByteSize, min_size);
    HRESULT hr = d.core->create_heap(type, size, flags, &ho.heap);
    VGD_TRACE("CreateHeap: %llu bytes (asked %llu), alignment %llu, type %d, flags 0x%x: 0x%08lx",
              static_cast<unsigned long long>(size), static_cast<unsigned long long>(heap.ByteSize),
              static_cast<unsigned long long>(heap.Alignment), static_cast<int>(type),
              static_cast<unsigned>(heap.Flags), hr);
    return hr;
}

// A heap alone.
HRESULT create_heap(Device& d, const D3D12DDIARG_CREATEHEAP_0001& heap, D3D12DDI_HHEAP hh, D3D12DDI_HRESOURCE hr_)
{
    auto* ho = new (hh.pDrvPrivate) HeapObject;
    if (FAILED(create_core_heap(d, heap, 0, *ho))) {
        ho->~HeapObject();
        return E_OUTOFMEMORY;
    }
    if (hr_.pDrvPrivate)
        new (hr_.pDrvPrivate) ResourceObject{ho, nullptr};
    return S_OK;
}

// A placed resource (CreatePlacedResource); reserved (tiled) resources name no heap and are not supported.
HRESULT create_placed(Device& d, const D3D12DDIARG_CREATERESOURCE_0088& resource, D3D12DDI_HRESOURCE hr_)
{
    const D3D12DDIARG_HRESOURCE_PLACEMENT& p = resource.ReuseBufferGPUVA.BaseAddress.UMD;
    const D3D12_RESOURCE_DESC desc = resource_desc(resource);
    const ResourceObject* base = p.hResource.pDrvPrivate ? object<ResourceObject>(p.hResource) : nullptr;
    HeapObject* heap = base ? (base->heap ? base->heap : base->placed_in) : nullptr;
    if (!heap || !heap->heap) {
        VGD_WARN("CreateHeapAndResource: %s resource (dimension %d, format %d) not supported",
                 base ? "placed in a committed" : "reserved", static_cast<int>(desc.Dimension),
                 static_cast<int>(desc.Format));
        return E_OUTOFMEMORY;
    }
    const uint64_t offset = base->offset + p.Offset;
    auto* ro = new (hr_.pDrvPrivate) ResourceObject;
    HRESULT hr = d.core->create_placed_resource(*heap->heap, offset, desc, &ro->placed);
    if (FAILED(hr)) {
        VGD_WARN("CreateHeapAndResource: placed dimension %d, %llux%ux%u, format %d, flags 0x%x at %llu failed: "
                 "0x%08lx",
                 static_cast<int>(desc.Dimension), static_cast<unsigned long long>(desc.Width), desc.Height,
                 desc.DepthOrArraySize, static_cast<int>(desc.Format), static_cast<unsigned>(desc.Flags),
                 static_cast<unsigned long long>(offset), hr);
        ro->~ResourceObject();
        return E_OUTOFMEMORY;
    }
    ro->resource = ro->placed.get();
    ro->placed_in = heap;
    ro->offset = offset;
    VGD_VERBOSE("CreateHeapAndResource: placed dimension %d, %llux%ux%u, format %d at %llu, va 0x%llx",
                static_cast<int>(desc.Dimension), static_cast<unsigned long long>(desc.Width), desc.Height,
                desc.DepthOrArraySize, static_cast<int>(desc.Format), static_cast<unsigned long long>(offset),
                static_cast<unsigned long long>(ro->placed->gpu_virtual_address()));
    return S_OK;
}

HRESULT APIENTRY create_heap_and_resource(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATEHEAP_0001* heap,
                                          D3D12DDI_HHEAP hh, D3D12DDI_HRTRESOURCE rt_resource,
                                          const D3D12DDIARG_CREATERESOURCE_0088* resource,
                                          const D3D12DDI_CLEAR_VALUES*, D3D12DDI_HPROTECTEDRESOURCESESSION_0030,
                                          D3D12DDI_HRESOURCE hr_)
{
    if (!heap && resource)
        return create_placed(*Device::from(h), *resource, hr_);
    if (!heap) {
        VGD_WARN("CreateHeapAndResource: neither a heap nor a resource description");
        return E_OUTOFMEMORY;
    }
    if (!resource)
        return create_heap(*Device::from(h), *heap, hh, hr_);
    const D3D12_HEAP_TYPE type = heap_type(*heap);
    const D3D12_RESOURCE_DESC desc = resource_desc(*resource);
    auto* ho = new (hh.pDrvPrivate) HeapObject;
    if (heap->Flags & D3D12DDI_HEAP_FLAG_PRIMARY) {
        // A swapchain buffer: the compositor reads it through a runtime allocation.
        HRESULT hr = create_primary(*Device::from(h), *heap, *resource, desc, rt_resource, *ho);
        if (FAILED(hr)) {
            ho->~HeapObject();
            return hr;
        }
        new (hr_.pDrvPrivate) ResourceObject{ho, ho->resource.get()};
        return S_OK;
    }
    // A buffer is a heap with the buffer placed at 0: CreateHeap reaches us as one (a buffer spanning the heap),
    // and resources are then placed in it by that buffer's handle. Textures keep memory of their own.
    HRESULT hr;
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        uint64_t size = 0, alignment = 0;
        hr = Device::from(h)->core->resource_allocation_info(desc, &size, &alignment);
        if (SUCCEEDED(hr))
            hr = create_core_heap(*Device::from(h), *heap, size, *ho);
        if (SUCCEEDED(hr))
            hr = Device::from(h)->core->create_placed_resource(*ho->heap, 0, desc, &ho->resource);
    } else {
        hr = Device::from(h)->core->create_committed_resource(type, desc, &ho->resource);
    }
    if (FAILED(hr)) {
        VGD_WARN("CreateHeapAndResource: dimension %d, %llux%ux%u, format %d, flags 0x%x, heap type %d failed: 0x%08lx",
                 static_cast<int>(desc.Dimension), static_cast<unsigned long long>(desc.Width), desc.Height,
                 desc.DepthOrArraySize, static_cast<int>(desc.Format), static_cast<unsigned>(desc.Flags),
                 static_cast<int>(type), hr);
        ho->~HeapObject();
        return E_OUTOFMEMORY;
    }
    new (hr_.pDrvPrivate) ResourceObject{ho, ho->resource.get()};
    VGD_VERBOSE("CreateHeapAndResource: dimension %d, %llux%ux%u, format %d, heap type %d, %llu bytes, va 0x%llx",
                static_cast<int>(desc.Dimension), static_cast<unsigned long long>(desc.Width), desc.Height,
                desc.DepthOrArraySize, static_cast<int>(desc.Format), static_cast<int>(type),
                static_cast<unsigned long long>(ho->resource->size()),
                static_cast<unsigned long long>(ho->resource->gpu_virtual_address()));
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

HRESULT APIENTRY map_heap(D3D12DDI_HDEVICE, D3D12DDI_HHEAP hh, void** data)
{
    auto* ho = object<HeapObject>(hh);
    // Placed resources are mapped through their heap: the runtime adds their offset.
    void* p = ho->heap ? ho->heap->cpu_pointer() : ho->resource ? ho->resource->cpu_pointer() : nullptr;
    if (!p) {
        VGD_WARN("MapHeap: heap has no CPU mapping");
        return E_INVALIDARG;
    }
    *data = p;
    VGD_VERBOSE("MapHeap: %p", p);
    return S_OK;
}

void APIENTRY unmap_heap(D3D12DDI_HDEVICE, D3D12DDI_HHEAP)
{
    // Mappings are persistent and the memory is host coherent.
}

D3D12DDI_GPU_VIRTUAL_ADDRESS APIENTRY check_resource_virtual_address(D3D12DDI_HDEVICE, D3D12DDI_HRESOURCE hr_)
{
    const auto* r = object<ResourceObject>(hr_);
    return r->resource ? r->resource->gpu_virtual_address() : 0;
}

// We keep no additional data next to a resource. Its alignments must still be powers of two: the runtime aligns its
// GetResourceAllocationInfo answer up to them, and zero wraps it to 0xFFFFFFFFFFFF0000 (measured in the guest).
void no_additional_data(D3D12DDI_RESOURCE_ALLOCATION_INFO_0022& info)
{
    info.AdditionalDataHeaderSize = 0;
    info.AdditionalDataSize = 0;
    info.AdditionalDataHeaderAlignment = info.ResourceDataAlignment;
    info.AdditionalDataAlignment = info.ResourceDataAlignment;
}

void APIENTRY check_resource_allocation_info(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATERESOURCE_0088* resource,
                                             D3D12DDI_RESOURCE_OPTIMIZATION_FLAGS optimization, UINT32 restriction,
                                             UINT, D3D12DDI_RESOURCE_ALLOCATION_INFO_0022* info)
{
    *info = {};
    const D3D12_RESOURCE_DESC desc = resource_desc(*resource);
    uint64_t size = 0, alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    if (FAILED(Device::from(h)->core->resource_allocation_info(desc, &size, &alignment))) {
        // Creation of this resource will fail too. A plausible answer keeps the runtime's bookkeeping consistent.
        VGD_WARN("CheckResourceAllocationInfo: dimension %d, format %d not supported", static_cast<int>(desc.Dimension),
                 static_cast<int>(desc.Format));
        size = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    }
    info->ResourceDataSize = size;
    info->ResourceDataAlignment = static_cast<UINT32>(alignment);
    no_additional_data(*info);
    info->Layout = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12DDI_TL_ROW_MAJOR : D3D12DDI_TL_UNDEFINED;
    static std::atomic<unsigned> calls{0};
    if (calls.fetch_add(1, std::memory_order_relaxed) < 16 || verbose())
        VGD_TRACE("CheckResourceAllocationInfo: dimension %d, %llux%ux%u, mips %u, format %d, flags 0x%x, layout %d, "
                  "optimization 0x%x, alignment restriction %u -> %llu bytes at %u",
                  static_cast<int>(desc.Dimension), static_cast<unsigned long long>(desc.Width), desc.Height,
                  desc.DepthOrArraySize, desc.MipLevels, static_cast<int>(desc.Format),
                  static_cast<unsigned>(resource->Flags), static_cast<int>(resource->Layout),
                  static_cast<unsigned>(optimization), restriction, static_cast<unsigned long long>(size),
                  info->ResourceDataAlignment);
}

void APIENTRY check_existing_resource_allocation_info(D3D12DDI_HDEVICE h, D3D12DDI_HRESOURCE hr_,
                                                      D3D12DDI_RESOURCE_ALLOCATION_INFO_0022* info)
{
    const auto* r = object<ResourceObject>(hr_);
    *info = {};
    uint64_t size = 0, alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    if (r->resource)
        Device::from(h)->core->resource_allocation_info(r->resource->desc(), &size, &alignment);
    info->ResourceDataSize = size;
    info->ResourceDataAlignment = static_cast<UINT32>(alignment);
    no_additional_data(*info);
    info->Layout = r->resource && r->resource->is_texture() ? D3D12DDI_TL_UNDEFINED : D3D12DDI_TL_ROW_MAJOR;
}

// Used by the runtime's Map to find a subresource within the heap mapping. A buffer is one subresource at 0.
void APIENTRY check_subresource_info(D3D12DDI_HDEVICE, D3D12DDI_HRESOURCE hr_, UINT subresource,
                                     D3D12DDI_SUBRESOURCE_INFO* info)
{
    const auto* r = object<ResourceObject>(hr_);
    *info = {};
    const UINT64 size = r->resource ? r->resource->size() : 0;
    info->RowStride = size;
    info->DepthStride = size;
    if (subresource != 0 || !r->resource || r->resource->is_texture())
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
    VGD_VERBOSE("CreateFence: value va 0x%llx, monitored va 0x%llx, flags 0x%x",
                static_cast<unsigned long long>(args->Fences[0].FenceValue.BaseAddress),
                static_cast<unsigned long long>(args->Fences[0].FenceMonitoredValue.BaseAddress),
                static_cast<unsigned>(args->Fences[0].Flags));
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
    new (hr_.pDrvPrivate) ResourceObject{ho, ho->resource.get()};
    return S_OK;
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
}

} // namespace vgd::umd
