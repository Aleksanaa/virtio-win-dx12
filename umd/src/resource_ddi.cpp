// SPDX-License-Identifier: MIT
// Heaps, resources, CPU mapping and fences. First version: committed buffers only, with memory owned by Vulkan
// (no WDDM allocations; residency is a no-op). Placed resources, heaps and textures come with the graphics milestone.
#include "device.h"
#include "log.h"
#include "objects.h"

#include <new>

namespace vgd::umd {

namespace {

constexpr UINT64 kBufferAlignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;   // 64 KiB

UINT64 align_up(UINT64 v, UINT64 a) { return (v + a - 1) & ~(a - 1); }

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

D3D12_RESOURCE_FLAGS api_flags(D3D12DDI_RESOURCE_FLAGS_0003 f)
{
    return (f & D3D12DDI_RESOURCE_FLAG_0022_UNORDERED_ACCESS) ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                                              : D3D12_RESOURCE_FLAG_NONE;
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
    if (!heap || !resource || resource->ResourceType != D3D12DDI_RT_BUFFER) {
        VGD_WARN("CreateHeapAndResource: only committed buffers so far (heap %p, resource %p, type %d)",
                 static_cast<const void*>(heap), static_cast<const void*>(resource),
                 resource ? static_cast<int>(resource->ResourceType) : -1);
        return E_OUTOFMEMORY;
    }
    const D3D12_HEAP_TYPE type = heap_type(*heap);
    auto* ho = new (hh.pDrvPrivate) HeapObject;
    HRESULT hr = Device::from(h)->core->create_committed_buffer(type, resource->Width, api_flags(resource->Flags),
                                                                &ho->buffer);
    if (FAILED(hr)) {
        VGD_WARN("CreateHeapAndResource: buffer of %llu bytes (heap type %d) failed: 0x%08lx",
                 static_cast<unsigned long long>(resource->Width), static_cast<int>(type), hr);
        ho->~HeapObject();
        return E_OUTOFMEMORY;
    }
    new (hr_.pDrvPrivate) ResourceObject{ho, ho->buffer.get()};
    VGD_VERBOSE("CreateHeapAndResource: buffer %llu bytes, heap type %d, va 0x%llx",
                static_cast<unsigned long long>(resource->Width), static_cast<int>(type),
                static_cast<unsigned long long>(ho->buffer->gpu_virtual_address()));
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
    void* p = ho->buffer ? ho->buffer->cpu_pointer() : nullptr;
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
    return r->buffer ? r->buffer->gpu_virtual_address() : 0;
}

void APIENTRY check_resource_allocation_info(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATERESOURCE_0088* resource,
                                             D3D12DDI_RESOURCE_OPTIMIZATION_FLAGS, UINT32, UINT,
                                             D3D12DDI_RESOURCE_ALLOCATION_INFO_0022* info)
{
    *info = {};
    info->Layout = D3D12DDI_TL_ROW_MAJOR;
    if (resource->ResourceType != D3D12DDI_RT_BUFFER) {
        // Textures come with the graphics milestone. A plausible, generous answer keeps the runtime's
        // bookkeeping consistent until then; creation itself still fails.
        VGD_WARN("CheckResourceAllocationInfo: texture type %d %llux%u not supported yet",
                 static_cast<int>(resource->ResourceType), static_cast<unsigned long long>(resource->Width),
                 resource->Height);
        info->Layout = D3D12DDI_TL_UNDEFINED;
        info->ResourceDataSize = align_up(resource->Width * resource->Height * resource->DepthOrArraySize * 16 * 2,
                                          kBufferAlignment);
        info->ResourceDataAlignment = static_cast<UINT32>(kBufferAlignment);
        return;
    }
    info->ResourceDataSize = align_up(resource->Width, kBufferAlignment);
    info->ResourceDataAlignment = static_cast<UINT32>(kBufferAlignment);
}

void APIENTRY check_existing_resource_allocation_info(D3D12DDI_HDEVICE, D3D12DDI_HRESOURCE hr_,
                                                      D3D12DDI_RESOURCE_ALLOCATION_INFO_0022* info)
{
    const auto* r = object<ResourceObject>(hr_);
    *info = {};
    info->Layout = D3D12DDI_TL_ROW_MAJOR;
    info->ResourceDataSize = align_up(r->buffer ? r->buffer->size() : 0, kBufferAlignment);
    info->ResourceDataAlignment = static_cast<UINT32>(kBufferAlignment);
}

// Used by the runtime's Map to find a subresource within the heap mapping. A buffer is one subresource at 0.
void APIENTRY check_subresource_info(D3D12DDI_HDEVICE, D3D12DDI_HRESOURCE hr_, UINT subresource,
                                     D3D12DDI_SUBRESOURCE_INFO* info)
{
    const auto* r = object<ResourceObject>(hr_);
    *info = {};
    const UINT64 size = r->buffer ? r->buffer->size() : 0;
    info->RowStride = size;
    info->DepthStride = size;
    if (subresource != 0 || !r->buffer)
        VGD_WARN("CheckSubresourceInfo: subresource %u of a non-buffer resource", subresource);
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
