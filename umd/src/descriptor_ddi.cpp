// SPDX-License-Identifier: MIT
// Descriptor heaps, views and samplers, on vkd3d-proton's descriptor heaps. The CPU and GPU handles the runtime
// hands out are vkd3d-proton's own (the heap starts come from it), so views are written straight to them; every
// Create*View turns the DDI's description into the API's.
#include "device.h"
#include "log.h"
#include "objects.h"

#include <cstddef>
#include <new>

namespace vgd::umd {

namespace {

D3D12_CPU_DESCRIPTOR_HANDLE cpu(D3D12DDI_CPU_DESCRIPTOR_HANDLE h)
{
    return {h.ptr};
}

SIZE_T APIENTRY calc_private_descriptor_heap_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001*)
{
    return sizeof(DescriptorHeapObject);
}

HRESULT APIENTRY create_descriptor_heap(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001* args,
                                        D3D12DDI_HDESCRIPTORHEAP hh)
{
    auto* obj = new (hh.pDrvPrivate) DescriptorHeapObject;
    const D3D12_DESCRIPTOR_HEAP_DESC desc{static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(args->Type), args->NumDescriptors,
                                          static_cast<D3D12_DESCRIPTOR_HEAP_FLAGS>(args->Flags), args->NodeMask};
    HRESULT hr = Device::from(h)->d3d->CreateDescriptorHeap(&desc, __uuidof(ID3D12DescriptorHeap), obj->heap.put_void());
    if (FAILED(hr)) {
        VGD_WARN("CreateDescriptorHeap: type %d, %u descriptors, flags 0x%x failed: 0x%08lx",
                 static_cast<int>(args->Type), args->NumDescriptors, static_cast<unsigned>(args->Flags), hr);
        obj->~DescriptorHeapObject();
        return E_OUTOFMEMORY;
    }
    return S_OK;
}

void APIENTRY destroy_descriptor_heap(D3D12DDI_HDEVICE, D3D12DDI_HDESCRIPTORHEAP hh)
{
    object<DescriptorHeapObject>(hh)->~DescriptorHeapObject();
}

D3D12DDI_CPU_DESCRIPTOR_HANDLE APIENTRY get_cpu_descriptor_handle_for_heap_start(D3D12DDI_HDEVICE,
                                                                                D3D12DDI_HDESCRIPTORHEAP hh)
{
    D3D12_CPU_DESCRIPTOR_HANDLE start;
    object<DescriptorHeapObject>(hh)->heap->GetCPUDescriptorHandleForHeapStart(&start);
    return {start.ptr};
}

D3D12DDI_GPU_DESCRIPTOR_HANDLE APIENTRY get_gpu_descriptor_handle_for_heap_start(D3D12DDI_HDEVICE,
                                                                                D3D12DDI_HDESCRIPTORHEAP hh)
{
    D3D12_GPU_DESCRIPTOR_HANDLE start;
    object<DescriptorHeapObject>(hh)->heap->GetGPUDescriptorHandleForHeapStart(&start);
    return {start.ptr};
}

bool multisampled(ID3D12Resource* r)
{
    return r && desc_of(r).SampleDesc.Count > 1;
}

// The DDI describes views per resource dimension, with array ranges; the API's array/non-array choice (Texture2D vs
// Texture2DArray), which Vulkan view types must match, does not reach the driver. A view is arrayed if it covers
// more than one slice or starts past the first, which is what the API's non-array forms cannot express.
bool arrayed(UINT first, UINT size)
{
    return first > 0 || (size != 1 && size != 0);
}

void APIENTRY create_shader_resource_view(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_SHADER_RESOURCE_VIEW_0002* a,
                                          D3D12DDI_CPU_DESCRIPTOR_HANDLE dest)
{
    ID3D12Resource* r = resource_of(a->hDrvResource);
    D3D12_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = a->Format;
    v.Shader4ComponentMapping = a->Shader4ComponentMapping;
    switch (a->ResourceDimension) {
    case D3D12DDI_RD_BUFFER:
        v.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        v.Buffer = {a->Buffer.FirstElement, a->Buffer.NumElements, a->Buffer.StructureByteStride,
                    (a->Buffer.Flags & D3D12DDI_BUFFER_SRV_FLAG_RAW) ? D3D12_BUFFER_SRV_FLAG_RAW
                                                                     : D3D12_BUFFER_SRV_FLAG_NONE};
        break;
    case D3D12DDI_RD_TEXTURE1D:
        if (arrayed(a->Tex1D.FirstArraySlice, a->Tex1D.ArraySize)) {
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
            v.Texture1DArray = {a->Tex1D.MostDetailedMip, a->Tex1D.MipLevels, a->Tex1D.FirstArraySlice,
                                a->Tex1D.ArraySize, a->Tex1D.ResourceMinLODClamp};
        } else {
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
            v.Texture1D = {a->Tex1D.MostDetailedMip, a->Tex1D.MipLevels, a->Tex1D.ResourceMinLODClamp};
        }
        break;
    case D3D12DDI_RD_TEXTURE2D: {
        const auto& t = a->Tex2D;
        if (multisampled(r) && arrayed(t.FirstArraySlice, t.ArraySize)) {
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
            v.Texture2DMSArray = {t.FirstArraySlice, t.ArraySize};
        } else if (multisampled(r)) {
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        } else if (arrayed(t.FirstArraySlice, t.ArraySize)) {
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            v.Texture2DArray = {t.MostDetailedMip, t.MipLevels, t.FirstArraySlice, t.ArraySize, t.PlaneSlice,
                                t.ResourceMinLODClamp};
        } else {
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            v.Texture2D = {t.MostDetailedMip, t.MipLevels, t.PlaneSlice, t.ResourceMinLODClamp};
        }
        break;
    }
    case D3D12DDI_RD_TEXTURE3D:
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        v.Texture3D = {a->Tex3D.MostDetailedMip, a->Tex3D.MipLevels, a->Tex3D.ResourceMinLODClamp};
        break;
    case D3D12DDI_RD_TEXTURECUBE: {
        const auto& t = a->TexCube;
        if (t.First2DArrayFace > 0 || (t.NumCubes != 1 && t.NumCubes != 0)) {
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
            v.TextureCubeArray = {t.MostDetailedMip, t.MipLevels, t.First2DArrayFace, t.NumCubes,
                                  t.ResourceMinLODClamp};
        } else {
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            v.TextureCube = {t.MostDetailedMip, t.MipLevels, t.ResourceMinLODClamp};
        }
        break;
    }
    case D3D12DDI_RD_RAYTRACING_ACCELERATION_STRUCTURE_0042:
        v.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        v.RaytracingAccelerationStructure = {a->RaytracingAccelerationStructure.Location};
        r = nullptr;
        break;
    default:
        VGD_WARN("CreateShaderResourceView: dimension %d not supported", static_cast<int>(a->ResourceDimension));
        return;
    }
    Device::from(h)->d3d->CreateShaderResourceView(r, &v, cpu(dest));
}

void APIENTRY create_unordered_access_view(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_UNORDERED_ACCESS_VIEW_0002* a,
                                           D3D12DDI_CPU_DESCRIPTOR_HANDLE dest)
{
    ID3D12Resource* counter = nullptr;
    D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
    v.Format = a->Format;
    switch (a->ResourceDimension) {
    case D3D12DDI_RD_BUFFER:
        v.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        v.Buffer = {a->Buffer.FirstElement, a->Buffer.NumElements, a->Buffer.StructureByteStride,
                    a->Buffer.CounterOffsetInBytes,
                    (a->Buffer.Flags & D3D12DDI_BUFFER_UAV_FLAG_RAW) ? D3D12_BUFFER_UAV_FLAG_RAW
                                                                     : D3D12_BUFFER_UAV_FLAG_NONE};
        counter = resource_of(a->Buffer.hDrvCounterResource);
        break;
    case D3D12DDI_RD_TEXTURE1D:
        if (arrayed(a->Tex1D.FirstArraySlice, a->Tex1D.ArraySize)) {
            v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
            v.Texture1DArray = {a->Tex1D.MipSlice, a->Tex1D.FirstArraySlice, a->Tex1D.ArraySize};
        } else {
            v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
            v.Texture1D = {a->Tex1D.MipSlice};
        }
        break;
    case D3D12DDI_RD_TEXTURE2D:
        if (arrayed(a->Tex2D.FirstArraySlice, a->Tex2D.ArraySize)) {
            v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            v.Texture2DArray = {a->Tex2D.MipSlice, a->Tex2D.FirstArraySlice, a->Tex2D.ArraySize, a->Tex2D.PlaneSlice};
        } else {
            v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            v.Texture2D = {a->Tex2D.MipSlice, a->Tex2D.PlaneSlice};
        }
        break;
    case D3D12DDI_RD_TEXTURE3D:
        v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        v.Texture3D = {a->Tex3D.MipSlice, a->Tex3D.FirstW, a->Tex3D.WSize};
        break;
    default:
        VGD_WARN("CreateUnorderedAccessView: dimension %d not supported", static_cast<int>(a->ResourceDimension));
        return;
    }
    Device::from(h)->d3d->CreateUnorderedAccessView(resource_of(a->hDrvResource), counter, &v, cpu(dest));
}

void APIENTRY create_constant_buffer_view(D3D12DDI_HDEVICE h, const D3D12DDI_CONSTANT_BUFFER_VIEW_DESC* a,
                                          D3D12DDI_CPU_DESCRIPTOR_HANDLE dest)
{
    const D3D12_CONSTANT_BUFFER_VIEW_DESC v{a->BufferLocation, a->SizeInBytes};
    Device::from(h)->d3d->CreateConstantBufferView(&v, cpu(dest));
}

// The DDI sampler description is the API's field for field.
static_assert(sizeof(D3D12DDI_SAMPLER_DESC) == sizeof(D3D12_SAMPLER_DESC));
static_assert(offsetof(D3D12DDI_SAMPLER_DESC, MaxLOD) == offsetof(D3D12_SAMPLER_DESC, MaxLOD));

void APIENTRY create_sampler(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_SAMPLER* a,
                             D3D12DDI_CPU_DESCRIPTOR_HANDLE dest)
{
    Device::from(h)->d3d->CreateSampler(reinterpret_cast<const D3D12_SAMPLER_DESC*>(a->pSamplerDesc), cpu(dest));
}

void APIENTRY create_depth_stencil_view(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_DEPTH_STENCIL_VIEW* a,
                                        D3D12DDI_CPU_DESCRIPTOR_HANDLE dest)
{
    ID3D12Resource* r = resource_of(a->hDrvResource);
    D3D12_DEPTH_STENCIL_VIEW_DESC v{};
    v.Format = a->Format;
    v.Flags = static_cast<D3D12_DSV_FLAGS>(a->Flags & D3D12DDI_CREATE_DSV_FLAG_MASK);   // same bits as the API
    switch (a->ResourceDimension) {
    case D3D12DDI_RD_TEXTURE1D:
        v.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY;
        v.Texture1DArray = {a->Tex1D.MipSlice, a->Tex1D.FirstArraySlice, a->Tex1D.ArraySize};
        break;
    case D3D12DDI_RD_TEXTURE2D:
    case D3D12DDI_RD_TEXTURECUBE:
        if (multisampled(r)) {
            v.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
            v.Texture2DMSArray = {a->Tex2D.FirstArraySlice, a->Tex2D.ArraySize};
        } else {
            v.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            v.Texture2DArray = {a->Tex2D.MipSlice, a->Tex2D.FirstArraySlice, a->Tex2D.ArraySize};
        }
        break;
    default:
        VGD_WARN("CreateDepthStencilView: dimension %d not supported", static_cast<int>(a->ResourceDimension));
        return;
    }
    Device::from(h)->d3d->CreateDepthStencilView(r, &v, cpu(dest));
}

void APIENTRY create_render_target_view(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002* a,
                                        D3D12DDI_CPU_DESCRIPTOR_HANDLE dest)
{
    ID3D12Resource* r = resource_of(a->hDrvResource);
    // The DDI has one form per dimension, with array ranges; the API's array and MS variants cover the same. A null
    // resource makes a null RTV (CreateRenderTargetView(nullptr, &desc)), whose writes are discarded.
    D3D12_RENDER_TARGET_VIEW_DESC v{};
    v.Format = a->Format;
    switch (a->ResourceDimension) {
    case D3D12DDI_RD_BUFFER:
        v.ViewDimension = D3D12_RTV_DIMENSION_BUFFER;
        v.Buffer = {a->Buffer.FirstElement, a->Buffer.NumElements};
        break;
    case D3D12DDI_RD_TEXTURE1D:
        v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
        v.Texture1DArray = {a->Tex1D.MipSlice, a->Tex1D.FirstArraySlice, a->Tex1D.ArraySize};
        break;
    case D3D12DDI_RD_TEXTURE2D:
        if (multisampled(r)) {
            v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
            v.Texture2DMSArray = {a->Tex2D.FirstArraySlice, a->Tex2D.ArraySize};
        } else {
            v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            v.Texture2DArray = {a->Tex2D.MipSlice, a->Tex2D.FirstArraySlice, a->Tex2D.ArraySize, a->Tex2D.PlaneSlice};
        }
        break;
    case D3D12DDI_RD_TEXTURECUBE:
        v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        v.Texture2DArray = {a->TexCube.MipSlice, a->TexCube.FirstArraySlice, a->TexCube.ArraySize, 0};
        break;
    case D3D12DDI_RD_TEXTURE3D:
        v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
        v.Texture3D = {a->Tex3D.MipSlice, a->Tex3D.FirstW, a->Tex3D.WSize};
        break;
    default:
        VGD_WARN("CreateRenderTargetView: dimension %d not supported", static_cast<int>(a->ResourceDimension));
        return;
    }
    Device::from(h)->d3d->CreateRenderTargetView(r, &v, cpu(dest));
}

// --- Copies --------------------------------------------------------------------------------------------------

void APIENTRY copy_descriptors(D3D12DDI_HDEVICE h, UINT num_dst_ranges, const D3D12DDI_CPU_DESCRIPTOR_HANDLE* dst_starts,
                               const UINT* dst_sizes, UINT num_src_ranges,
                               const D3D12DDI_CPU_DESCRIPTOR_HANDLE* src_starts, const UINT* src_sizes,
                               D3D12DDI_DESCRIPTOR_HEAP_TYPE type)
{
    static_assert(sizeof(D3D12DDI_CPU_DESCRIPTOR_HANDLE) == sizeof(D3D12_CPU_DESCRIPTOR_HANDLE));
    Device::from(h)->d3d->CopyDescriptors(num_dst_ranges,
                                          reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(dst_starts), dst_sizes,
                                          num_src_ranges,
                                          reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(src_starts), src_sizes,
                                          static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(type));
}

void APIENTRY copy_descriptors_simple(D3D12DDI_HDEVICE h, UINT count, D3D12DDI_CPU_DESCRIPTOR_HANDLE dst,
                                      D3D12DDI_CPU_DESCRIPTOR_HANDLE src, D3D12DDI_DESCRIPTOR_HEAP_TYPE type)
{
    Device::from(h)->d3d->CopyDescriptorsSimple(count, cpu(dst), cpu(src), static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(type));
}

} // namespace

void fill_descriptor_ddi(D3D12DDI_DEVICE_FUNCS_CORE_0088& t)
{
    t.pfnCalcPrivateDescriptorHeapSize = calc_private_descriptor_heap_size;
    t.pfnCreateDescriptorHeap = create_descriptor_heap;
    t.pfnDestroyDescriptorHeap = destroy_descriptor_heap;
    t.pfnGetCPUDescriptorHandleForHeapStart = get_cpu_descriptor_handle_for_heap_start;
    t.pfnGetGPUDescriptorHandleForHeapStart = get_gpu_descriptor_handle_for_heap_start;
    t.pfnCreateShaderResourceView = create_shader_resource_view;
    t.pfnCreateUnorderedAccessView = create_unordered_access_view;
    t.pfnCreateConstantBufferView = create_constant_buffer_view;
    t.pfnCreateSampler = create_sampler;
    t.pfnCreateRenderTargetView = create_render_target_view;
    t.pfnCreateDepthStencilView = create_depth_stencil_view;
    t.pfnCopyDescriptors = copy_descriptors;
    t.pfnCopyDescriptorsSimple = copy_descriptors_simple;
}

} // namespace vgd::umd
