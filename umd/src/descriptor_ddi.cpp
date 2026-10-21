// SPDX-License-Identifier: MIT
// Descriptor heaps, views and samplers, on core descriptor heaps (core/src/descriptors.cpp). Every Create*View
// turns the DDI's description into the API's, lets core make the descriptor and stores it at the destination
// handle; core also writes it to the heap's Vulkan descriptor set when the heap is shader-visible.
#include "device.h"
#include "log.h"
#include "objects.h"

#include <algorithm>
#include <cstddef>
#include <new>

namespace vgd::umd {

namespace {

SIZE_T APIENTRY calc_private_descriptor_heap_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001*)
{
    return sizeof(DescriptorHeapObject);
}

HRESULT APIENTRY create_descriptor_heap(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001* args,
                                        D3D12DDI_HDESCRIPTORHEAP hh)
{
    auto* obj = new (hh.pDrvPrivate) DescriptorHeapObject;
    const bool visible = args->Flags & D3D12DDI_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HRESULT hr = Device::from(h)->core->create_descriptor_heap(static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(args->Type),
                                                               args->NumDescriptors, visible, &obj->heap);
    if (FAILED(hr)) {
        VGD_WARN("CreateDescriptorHeap: type %d, %u descriptors, flags 0x%x failed: 0x%08lx",
                 static_cast<int>(args->Type), args->NumDescriptors, static_cast<unsigned>(args->Flags), hr);
        obj->~DescriptorHeapObject();
        return E_OUTOFMEMORY;
    }
    VGD_VERBOSE("CreateDescriptorHeap: type %d, %u descriptors, flags 0x%x", static_cast<int>(args->Type),
                args->NumDescriptors, static_cast<unsigned>(args->Flags));
    return S_OK;
}

void APIENTRY destroy_descriptor_heap(D3D12DDI_HDEVICE, D3D12DDI_HDESCRIPTORHEAP hh)
{
    object<DescriptorHeapObject>(hh)->~DescriptorHeapObject();
}

D3D12DDI_CPU_DESCRIPTOR_HANDLE APIENTRY get_cpu_descriptor_handle_for_heap_start(D3D12DDI_HDEVICE,
                                                                                D3D12DDI_HDESCRIPTORHEAP hh)
{
    return {reinterpret_cast<SIZE_T>(object<DescriptorHeapObject>(hh)->heap->cpu_start())};
}

D3D12DDI_GPU_DESCRIPTOR_HANDLE APIENTRY get_gpu_descriptor_handle_for_heap_start(D3D12DDI_HDEVICE,
                                                                                D3D12DDI_HDESCRIPTORHEAP hh)
{
    return {object<DescriptorHeapObject>(hh)->heap->gpu_start()};
}

vgd::Resource* resource_of(D3D12DDI_HRESOURCE h)
{
    return h.pDrvPrivate ? object<ResourceObject>(h)->resource : nullptr;
}

// Stores a view, or a null descriptor of the view's kind if making it failed (so a later draw reads nothing rather
// than a stale view).
void store(Device* d, D3D12DDI_CPU_DESCRIPTOR_HANDLE dest, HRESULT hr, const vgd::Descriptor& value, const char* what)
{
    vgd::Descriptor v = value;
    if (FAILED(hr)) {
        VGD_WARN("Create%s failed: 0x%08lx", what, hr);
        const auto kind = v.kind;
        v = {};
        v.kind = kind;
    }
    d->core->write_descriptor(descriptor_at(dest), v);
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
    Device* d = Device::from(h);
    vgd::Resource* r = resource_of(a->hDrvResource);
    D3D12_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = a->Format;
    v.Shader4ComponentMapping = a->Shader4ComponentMapping;
    const bool ms = r && r->desc().SampleDesc.Count > 1;
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
        if (ms && arrayed(t.FirstArraySlice, t.ArraySize)) {
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
            v.Texture2DMSArray = {t.FirstArraySlice, t.ArraySize};
        } else if (ms) {
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
    default:
        VGD_WARN("CreateShaderResourceView: dimension %d not supported", static_cast<int>(a->ResourceDimension));
        store(d, dest, E_INVALIDARG, {}, "ShaderResourceView");
        return;
    }
    vgd::Descriptor out;
    HRESULT hr = d->core->create_shader_resource_view(r, &v, &out);
    store(d, dest, hr, out, "ShaderResourceView");
}

void APIENTRY create_unordered_access_view(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_UNORDERED_ACCESS_VIEW_0002* a,
                                           D3D12DDI_CPU_DESCRIPTOR_HANDLE dest)
{
    Device* d = Device::from(h);
    vgd::Resource* r = resource_of(a->hDrvResource);
    D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
    v.Format = a->Format;
    switch (a->ResourceDimension) {
    case D3D12DDI_RD_BUFFER:
        v.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        v.Buffer = {a->Buffer.FirstElement, a->Buffer.NumElements, a->Buffer.StructureByteStride,
                    a->Buffer.CounterOffsetInBytes,
                    (a->Buffer.Flags & D3D12DDI_BUFFER_UAV_FLAG_RAW) ? D3D12_BUFFER_UAV_FLAG_RAW
                                                                     : D3D12_BUFFER_UAV_FLAG_NONE};
        if (a->Buffer.hDrvCounterResource.pDrvPrivate)
            VGD_WARN("CreateUnorderedAccessView: UAV counters are not supported yet");
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
        store(d, dest, E_INVALIDARG, {}, "UnorderedAccessView");
        return;
    }
    vgd::Descriptor out;
    HRESULT hr = d->core->create_unordered_access_view(r, &v, &out);
    store(d, dest, hr, out, "UnorderedAccessView");
}

void APIENTRY create_constant_buffer_view(D3D12DDI_HDEVICE h, const D3D12DDI_CONSTANT_BUFFER_VIEW_DESC* a,
                                          D3D12DDI_CPU_DESCRIPTOR_HANDLE dest)
{
    Device* d = Device::from(h);
    vgd::Descriptor out;
    HRESULT hr = d->core->create_constant_buffer_view({a->BufferLocation, a->SizeInBytes}, &out);
    store(d, dest, hr, out, "ConstantBufferView");
}

// The DDI sampler description is the API's field for field.
static_assert(sizeof(D3D12DDI_SAMPLER_DESC) == sizeof(D3D12_SAMPLER_DESC));
static_assert(offsetof(D3D12DDI_SAMPLER_DESC, MaxLOD) == offsetof(D3D12_SAMPLER_DESC, MaxLOD));

void APIENTRY create_sampler(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_SAMPLER* a,
                             D3D12DDI_CPU_DESCRIPTOR_HANDLE dest)
{
    Device* d = Device::from(h);
    vgd::Descriptor out;
    HRESULT hr = d->core->create_sampler(*reinterpret_cast<const D3D12_SAMPLER_DESC*>(a->pSamplerDesc), &out);
    store(d, dest, hr, out, "Sampler");
}

void APIENTRY create_depth_stencil_view(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_DEPTH_STENCIL_VIEW* args,
                                        D3D12DDI_CPU_DESCRIPTOR_HANDLE dest)
{
    Device* dev = Device::from(h);
    vgd::Descriptor d;
    d.kind = vgd::Descriptor::Kind::dsv;
    auto* r = object<ResourceObject>(args->hDrvResource);
    if (!r || !r->resource || !r->resource->is_texture()) {
        store(dev, dest, E_INVALIDARG, d, "DepthStencilView");
        return;
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC v{};
    v.Format = args->Format;
    v.Flags = static_cast<D3D12_DSV_FLAGS>(args->Flags & D3D12DDI_CREATE_DSV_FLAG_MASK);   // same bits as the API
    const bool ms = r->resource->desc().SampleDesc.Count > 1;
    switch (args->ResourceDimension) {
    case D3D12DDI_RD_TEXTURE1D:
        v.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY;
        v.Texture1DArray = {args->Tex1D.MipSlice, args->Tex1D.FirstArraySlice, args->Tex1D.ArraySize};
        break;
    case D3D12DDI_RD_TEXTURE2D:
    case D3D12DDI_RD_TEXTURECUBE:
        if (ms) {
            v.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
            v.Texture2DMSArray = {args->Tex2D.FirstArraySlice, args->Tex2D.ArraySize};
        } else {
            v.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            v.Texture2DArray = {args->Tex2D.MipSlice, args->Tex2D.FirstArraySlice, args->Tex2D.ArraySize};
        }
        break;
    default:
        VGD_WARN("CreateDepthStencilView: dimension %d not supported", static_cast<int>(args->ResourceDimension));
        store(dev, dest, E_INVALIDARG, d, "DepthStencilView");
        return;
    }
    HRESULT hr = dev->core->create_depth_stencil_view(*r->resource, &v, &d.dsv);
    store(dev, dest, hr, d, "DepthStencilView");
}

void APIENTRY create_render_target_view(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002* args,
                                        D3D12DDI_CPU_DESCRIPTOR_HANDLE dest)
{
    Device* dev = Device::from(h);
    vgd::Descriptor d;
    d.kind = vgd::Descriptor::Kind::rtv;
    // A null resource makes a null RTV (CreateRenderTargetView(nullptr, &desc)): writes to it are discarded.
    if (!args->hDrvResource.pDrvPrivate) {
        store(dev, dest, S_OK, d, "RenderTargetView");
        return;
    }
    auto* r = object<ResourceObject>(args->hDrvResource);
    if (!r->resource || !r->resource->is_texture()) {
        VGD_WARN("CreateRenderTargetView: dimension %d on %s", static_cast<int>(args->ResourceDimension),
                 r->resource ? "a buffer (not supported)" : "a heap");
        store(dev, dest, E_INVALIDARG, d, "RenderTargetView");
        return;
    }
    // The DDI has one form per dimension, with array ranges; the API's array and MS variants cover the same.
    D3D12_RENDER_TARGET_VIEW_DESC v{};
    v.Format = args->Format;
    const bool ms = r->resource->desc().SampleDesc.Count > 1;
    switch (args->ResourceDimension) {
    case D3D12DDI_RD_TEXTURE1D:
        v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
        v.Texture1DArray = {args->Tex1D.MipSlice, args->Tex1D.FirstArraySlice, args->Tex1D.ArraySize};
        break;
    case D3D12DDI_RD_TEXTURE2D:
        if (ms) {
            v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
            v.Texture2DMSArray = {args->Tex2D.FirstArraySlice, args->Tex2D.ArraySize};
        } else {
            v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            v.Texture2DArray = {args->Tex2D.MipSlice, args->Tex2D.FirstArraySlice, args->Tex2D.ArraySize,
                                args->Tex2D.PlaneSlice};
        }
        break;
    case D3D12DDI_RD_TEXTURECUBE:
        v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        v.Texture2DArray = {args->TexCube.MipSlice, args->TexCube.FirstArraySlice, args->TexCube.ArraySize, 0};
        break;
    case D3D12DDI_RD_TEXTURE3D:
        v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
        v.Texture3D = {args->Tex3D.MipSlice, args->Tex3D.FirstW, args->Tex3D.WSize};
        break;
    default:
        VGD_WARN("CreateRenderTargetView: dimension %d not supported", static_cast<int>(args->ResourceDimension));
        store(dev, dest, E_INVALIDARG, d, "RenderTargetView");
        return;
    }
    HRESULT hr = dev->core->create_render_target_view(*r->resource, &v, &d.rtv);
    store(dev, dest, hr, d, "RenderTargetView");
}

// --- Copies --------------------------------------------------------------------------------------------------

void APIENTRY copy_descriptors(D3D12DDI_HDEVICE h, UINT num_dst_ranges, const D3D12DDI_CPU_DESCRIPTOR_HANDLE* dst_starts,
                               const UINT* dst_sizes, UINT num_src_ranges,
                               const D3D12DDI_CPU_DESCRIPTOR_HANDLE* src_starts, const UINT* src_sizes,
                               D3D12DDI_DESCRIPTOR_HEAP_TYPE)
{
    vgd::Device& core = *Device::from(h)->core;
    UINT di = 0, si = 0, d_off = 0, s_off = 0;
    while (di < num_dst_ranges && si < num_src_ranges) {
        const UINT d_size = dst_sizes ? dst_sizes[di] : 1, s_size = src_sizes ? src_sizes[si] : 1;
        const UINT n = std::min(d_size - d_off, s_size - s_off);
        if (n)
            core.copy_descriptors(descriptor_at(dst_starts[di]) + d_off, descriptor_at(src_starts[si]) + s_off, n);
        d_off += n;
        s_off += n;
        if (d_off >= d_size) {
            di++;
            d_off = 0;
        }
        if (s_off >= s_size) {
            si++;
            s_off = 0;
        }
    }
}

void APIENTRY copy_descriptors_simple(D3D12DDI_HDEVICE h, UINT count, D3D12DDI_CPU_DESCRIPTOR_HANDLE dst,
                                      D3D12DDI_CPU_DESCRIPTOR_HANDLE src, D3D12DDI_DESCRIPTOR_HEAP_TYPE)
{
    Device::from(h)->core->copy_descriptors(descriptor_at(dst), descriptor_at(src), count);
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
