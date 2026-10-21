// SPDX-License-Identifier: MIT
// Heaps, resources, CPU mapping and fences. First version: committed buffers and textures, with memory owned by
// Vulkan (no WDDM allocations; residency is a no-op). Placed resources and heap-only creation come later.
#include "device.h"
#include "log.h"
#include "objects.h"

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
    D3D12DDI_HDEVICE, const D3D12DDIARG_CREATEHEAP_0001* heap, const D3D12DDIARG_CREATERESOURCE_0088* resource,
    D3D12DDI_HPROTECTEDRESOURCESESSION_0030)
{
    return {heap ? sizeof(HeapObject) : 0, resource ? sizeof(ResourceObject) : 0};
}

HRESULT APIENTRY create_heap_and_resource(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATEHEAP_0001* heap,
                                          D3D12DDI_HHEAP hh, D3D12DDI_HRTRESOURCE,
                                          const D3D12DDIARG_CREATERESOURCE_0088* resource,
                                          const D3D12DDI_CLEAR_VALUES*, D3D12DDI_HPROTECTEDRESOURCESESSION_0030,
                                          D3D12DDI_HRESOURCE hr_)
{
    if (!heap || !resource) {
        VGD_WARN("CreateHeapAndResource: only committed resources so far (heap %p, resource %p)",
                 static_cast<const void*>(heap), static_cast<const void*>(resource));
        return E_OUTOFMEMORY;
    }
    const D3D12_HEAP_TYPE type = heap_type(*heap);
    const D3D12_RESOURCE_DESC desc = resource_desc(*resource);
    auto* ho = new (hh.pDrvPrivate) HeapObject;
    HRESULT hr = Device::from(h)->core->create_committed_resource(type, desc, &ho->resource);
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

void APIENTRY destroy_heap_and_resource(D3D12DDI_HDEVICE, D3D12DDI_HHEAP hh, D3D12DDI_HRESOURCE hr_)
{
    if (hr_.pDrvPrivate)
        object<ResourceObject>(hr_)->~ResourceObject();
    if (hh.pDrvPrivate)
        object<HeapObject>(hh)->~HeapObject();
}

HRESULT APIENTRY map_heap(D3D12DDI_HDEVICE, D3D12DDI_HHEAP hh, void** data)
{
    auto* ho = object<HeapObject>(hh);
    void* p = ho->resource ? ho->resource->cpu_pointer() : nullptr;
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

void APIENTRY check_resource_allocation_info(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATERESOURCE_0088* resource,
                                             D3D12DDI_RESOURCE_OPTIMIZATION_FLAGS, UINT32, UINT,
                                             D3D12DDI_RESOURCE_ALLOCATION_INFO_0022* info)
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
    info->Layout = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12DDI_TL_ROW_MAJOR : D3D12DDI_TL_UNDEFINED;
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

} // namespace

void fill_resource_ddi(D3D12DDI_DEVICE_FUNCS_CORE_0088& t)
{
    t.pfnCalcPrivateHeapAndResourceSizes = calc_private_heap_and_resource_sizes;
    t.pfnCreateHeapAndResource = create_heap_and_resource;
    t.pfnDestroyHeapAndResource = destroy_heap_and_resource;
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
