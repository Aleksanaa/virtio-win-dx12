// SPDX-License-Identifier: MIT
// Descriptor heaps and views. First version: render target views in CPU-side heaps. Shader-visible heaps exist (the
// runtime creates them), but nothing reads them until descriptor tables are mapped.
#include "device.h"
#include "log.h"
#include "objects.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace vgd::umd {

namespace {

SIZE_T APIENTRY calc_private_descriptor_heap_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001*)
{
    return sizeof(DescriptorHeapObject);
}

HRESULT APIENTRY create_descriptor_heap(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001* args,
                                        D3D12DDI_HDESCRIPTORHEAP hh)
{
    auto* obj = new (hh.pDrvPrivate) DescriptorHeapObject{args->Type, args->NumDescriptors,
                                                         (args->Flags & D3D12DDI_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0,
                                                         nullptr};
    const size_t bytes = size_t(args->NumDescriptors ? args->NumDescriptors : 1) * kDescriptorSize;
    obj->memory.reset(new (std::nothrow) std::byte[bytes]());
    if (!obj->memory) {
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
    return {reinterpret_cast<SIZE_T>(object<DescriptorHeapObject>(hh)->memory.get())};
}

// GPU handles only exist for shader-visible heaps. Until descriptor tables are mapped they are the CPU address too,
// which keeps them unique and lets a table argument be traced back to its heap.
D3D12DDI_GPU_DESCRIPTOR_HANDLE APIENTRY get_gpu_descriptor_handle_for_heap_start(D3D12DDI_HDEVICE,
                                                                                D3D12DDI_HDESCRIPTORHEAP hh)
{
    const auto* h = object<DescriptorHeapObject>(hh);
    return {h->shader_visible ? reinterpret_cast<UINT64>(h->memory.get()) : 0};
}

// --- Views ---------------------------------------------------------------------------------------------------

void APIENTRY create_render_target_view(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002* args,
                                        D3D12DDI_CPU_DESCRIPTOR_HANDLE dest)
{
    Descriptor* d = Descriptor::at(dest);
    *d = {};
    auto* r = object<ResourceObject>(args->hDrvResource);
    if (!r || !r->resource || !r->resource->is_texture()) {
        VGD_WARN("CreateRenderTargetView: dimension %d of a buffer is not supported", static_cast<int>(args->ResourceDimension));
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
        return;
    }
    HRESULT hr = Device::from(h)->core->create_render_target_view(*r->resource, &v, &d->rtv);
    if (FAILED(hr)) {
        VGD_WARN("CreateRenderTargetView: format %d, dimension %d failed: 0x%08lx", static_cast<int>(args->Format),
                 static_cast<int>(args->ResourceDimension), hr);
        d->rtv = {};
        return;
    }
    d->kind = Descriptor::Kind::rtv;
}

// --- Copies --------------------------------------------------------------------------------------------------
// Descriptors are plain data, so copying them is copying bytes.

void APIENTRY copy_descriptors(D3D12DDI_HDEVICE, UINT num_dst_ranges, const D3D12DDI_CPU_DESCRIPTOR_HANDLE* dst_starts,
                               const UINT* dst_sizes, UINT num_src_ranges,
                               const D3D12DDI_CPU_DESCRIPTOR_HANDLE* src_starts, const UINT* src_sizes,
                               D3D12DDI_DESCRIPTOR_HEAP_TYPE)
{
    UINT di = 0, si = 0, d_off = 0, s_off = 0;
    while (di < num_dst_ranges && si < num_src_ranges) {
        const UINT d_size = dst_sizes ? dst_sizes[di] : 1, s_size = src_sizes ? src_sizes[si] : 1;
        const UINT n = std::min(d_size - d_off, s_size - s_off);
        std::memmove(reinterpret_cast<void*>(dst_starts[di].ptr + SIZE_T(d_off) * kDescriptorSize),
                     reinterpret_cast<const void*>(src_starts[si].ptr + SIZE_T(s_off) * kDescriptorSize),
                     SIZE_T(n) * kDescriptorSize);
        d_off += n;
        s_off += n;
        if (d_off == d_size) {
            di++;
            d_off = 0;
        }
        if (s_off == s_size) {
            si++;
            s_off = 0;
        }
    }
}

void APIENTRY copy_descriptors_simple(D3D12DDI_HDEVICE, UINT count, D3D12DDI_CPU_DESCRIPTOR_HANDLE dst,
                                      D3D12DDI_CPU_DESCRIPTOR_HANDLE src, D3D12DDI_DESCRIPTOR_HEAP_TYPE)
{
    std::memmove(reinterpret_cast<void*>(dst.ptr), reinterpret_cast<const void*>(src.ptr), SIZE_T(count) * kDescriptorSize);
}

} // namespace

void fill_descriptor_ddi(D3D12DDI_DEVICE_FUNCS_CORE_0088& t)
{
    t.pfnCalcPrivateDescriptorHeapSize = calc_private_descriptor_heap_size;
    t.pfnCreateDescriptorHeap = create_descriptor_heap;
    t.pfnDestroyDescriptorHeap = destroy_descriptor_heap;
    t.pfnGetCPUDescriptorHandleForHeapStart = get_cpu_descriptor_handle_for_heap_start;
    t.pfnGetGPUDescriptorHandleForHeapStart = get_gpu_descriptor_handle_for_heap_start;
    t.pfnCreateRenderTargetView = create_render_target_view;
    t.pfnCopyDescriptors = copy_descriptors;
    t.pfnCopyDescriptorsSimple = copy_descriptors_simple;
}

} // namespace vgd::umd
