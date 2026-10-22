// SPDX-License-Identifier: MIT
// Resources: creation (ordinary ones on DXVK, shared surfaces and primaries in present.cpp), mapping, copies and
// updates.
#include "device.h"

#include <algorithm>

namespace vgd::umd {

UINT api_bind_flags(UINT b)
{
    // The first seven pipeline bits have the API's values; unordered access and the video bits do not.
    UINT out = b & (D3D10_DDI_BIND_VERTEX_BUFFER | D3D10_DDI_BIND_INDEX_BUFFER | D3D10_DDI_BIND_CONSTANT_BUFFER |
                    D3D10_DDI_BIND_SHADER_RESOURCE | D3D10_DDI_BIND_STREAM_OUTPUT | D3D10_DDI_BIND_RENDER_TARGET |
                    D3D10_DDI_BIND_DEPTH_STENCIL);
    if (b & D3D11_DDI_BIND_UNORDERED_ACCESS)
        out |= D3D11_BIND_UNORDERED_ACCESS;
    if (b & D3D11_DDI_BIND_DECODER)
        out |= D3D11_BIND_DECODER;
    if (b & D3D11_DDI_BIND_VIDEO_ENCODER)
        out |= D3D11_BIND_VIDEO_ENCODER;
    return out;
}

D3D11_BOX api_box(const D3D10_DDI_BOX& b)
{
    return {static_cast<UINT>(b.left), static_cast<UINT>(b.top), static_cast<UINT>(b.front),
            static_cast<UINT>(b.right), static_cast<UINT>(b.bottom), static_cast<UINT>(b.back)};
}

namespace {

UINT api_cpu_access(UINT map_flags)
{
    UINT out = 0;
    if (map_flags & D3D10_DDI_CPU_ACCESS_WRITE)
        out |= D3D11_CPU_ACCESS_WRITE;
    if (map_flags & D3D10_DDI_CPU_ACCESS_READ)
        out |= D3D11_CPU_ACCESS_READ;
    return out;
}

UINT api_misc_flags(const D3D11DDIARG_CREATERESOURCE& a)
{
    // These have the API's values; sharing is ours (present.cpp), the rest is the runtime's business.
    UINT out = a.MiscFlags & (D3D10_DDI_RESOURCE_AUTO_GEN_MIP_MAP | D3D11_DDI_RESOURCE_MISC_DRAWINDIRECT_ARGS |
                              D3D11_DDI_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS |
                              D3D11_DDI_RESOURCE_MISC_BUFFER_STRUCTURED | D3D11_DDI_RESOURCE_MISC_RESOURCE_CLAMP);
    if (a.ResourceDimension == D3D10DDIRESOURCE_TEXTURECUBE)
        out |= D3D11_RESOURCE_MISC_TEXTURECUBE;
    return out;
}

SIZE_T APIENTRY calc_private_resource_size(D3D10DDI_HDEVICE, const D3D11DDIARG_CREATERESOURCE*)
{
    return sizeof(ResourceObject);
}

SIZE_T APIENTRY calc_private_opened_resource_size(D3D10DDI_HDEVICE, const D3D10DDIARG_OPENRESOURCE*)
{
    return sizeof(ResourceObject);
}

HRESULT create_ordinary(Device& d, const D3D11DDIARG_CREATERESOURCE& a, ResourceObject& r)
{
    const auto* init = reinterpret_cast<const D3D11_SUBRESOURCE_DATA*>(a.pInitialDataUP);
    static_assert(sizeof(D3D11_SUBRESOURCE_DATA) == sizeof(D3D10_DDIARG_SUBRESOURCE_UP));
    const D3D11_USAGE usage = static_cast<D3D11_USAGE>(a.Usage);
    const UINT bind = api_bind_flags(a.BindFlags), cpu = api_cpu_access(a.MapFlags), misc = api_misc_flags(a);
    const D3D10DDI_MIPINFO& mip0 = a.pMipInfoList[0];
    switch (a.ResourceDimension) {
    case D3D10DDIRESOURCE_BUFFER:
    case D3D11DDIRESOURCE_BUFFEREX: {
        D3D11_BUFFER_DESC desc{mip0.TexelWidth, usage, bind, cpu, misc, a.ByteStride};
        return d.d3d->CreateBuffer(&desc, init, reinterpret_cast<ID3D11Buffer**>(r.resource.put()));
    }
    case D3D10DDIRESOURCE_TEXTURE1D: {
        D3D11_TEXTURE1D_DESC desc{mip0.TexelWidth, a.MipLevels, a.ArraySize, a.Format, usage, bind, cpu, misc};
        return d.d3d->CreateTexture1D(&desc, init, reinterpret_cast<ID3D11Texture1D**>(r.resource.put()));
    }
    case D3D10DDIRESOURCE_TEXTURE2D:
    case D3D10DDIRESOURCE_TEXTURECUBE: {
        // A cube's ArraySize counts faces, as the API's does.
        D3D11_TEXTURE2D_DESC1 desc{mip0.TexelWidth, mip0.TexelHeight, a.MipLevels, a.ArraySize, a.Format,
                                   a.SampleDesc, usage, bind, cpu, misc, D3D11_TEXTURE_LAYOUT_UNDEFINED};
        return d.d3d->CreateTexture2D1(&desc, init, reinterpret_cast<ID3D11Texture2D1**>(r.resource.put()));
    }
    case D3D10DDIRESOURCE_TEXTURE3D: {
        D3D11_TEXTURE3D_DESC1 desc{mip0.TexelWidth, mip0.TexelHeight, mip0.TexelDepth, a.MipLevels, a.Format,
                                   usage, bind, cpu, misc, D3D11_TEXTURE_LAYOUT_UNDEFINED};
        return d.d3d->CreateTexture3D1(&desc, init, reinterpret_cast<ID3D11Texture3D1**>(r.resource.put()));
    }
    default: return E_INVALIDARG;
    }
}

void APIENTRY create_resource(D3D10DDI_HDEVICE h, const D3D11DDIARG_CREATERESOURCE* a, D3D10DDI_HRESOURCE hr_,
                              D3D10DDI_HRTRESOURCE rt)
{
    Device* d = Device::from(h);
    auto* r = new (hr_.pDrvPrivate) ResourceObject;
    r->dimension = a->ResourceDimension;
    r->format = a->Format;
    r->bind = a->BindFlags;
    r->misc = a->MiscFlags;
    r->cpu = a->MapFlags;
    r->width = a->pMipInfoList[0].TexelWidth;
    r->height = a->pMipInfoList[0].TexelHeight;
    r->mips = a->MipLevels;
    r->array_size = a->ArraySize;
    r->samples = a->SampleDesc.Count;
    r->rt_resource = rt;
    const bool shared = is_shared_surface(*a);
    HRESULT hr = shared ? create_shared_surface(*d, *a, rt, *r) : create_ordinary(*d, *a, *r);
    if (FAILED(hr) || verbose() || shared)
        (FAILED(hr) ? logf(LogLevel::warn, "CreateResource: dimension %d %ux%ux%u, %u mips, %u layers, format %d, "
                                           "%u samples, usage %u, bind 0x%x, cpu 0x%x, misc 0x%x%s: 0x%08lx",
                           a->ResourceDimension, r->width, r->height, a->pMipInfoList[0].TexelDepth, a->MipLevels,
                           a->ArraySize, a->Format, a->SampleDesc.Count, a->Usage, a->BindFlags, a->MapFlags,
                           a->MiscFlags, a->pPrimaryDesc ? ", primary" : "", hr)
                    : logf(LogLevel::debug, "CreateResource: dimension %d %ux%ux%u, %u mips, %u layers, format %d, "
                                            "%u samples, usage %u, bind 0x%x, cpu 0x%x, misc 0x%x%s",
                           a->ResourceDimension, r->width, r->height, a->pMipInfoList[0].TexelDepth, a->MipLevels,
                           a->ArraySize, a->Format, a->SampleDesc.Count, a->Usage, a->BindFlags, a->MapFlags,
                           a->MiscFlags, a->pPrimaryDesc ? ", primary" : ""));
    if (FAILED(hr))
        d->set_error(E_OUTOFMEMORY);
}

void APIENTRY open_resource(D3D10DDI_HDEVICE h, const D3D10DDIARG_OPENRESOURCE* a, D3D10DDI_HRESOURCE hr_,
                            D3D10DDI_HRTRESOURCE rt)
{
    Device* d = Device::from(h);
    auto* r = new (hr_.pDrvPrivate) ResourceObject;
    r->rt_resource = rt;
    HRESULT hr = open_shared_surface(*d, *a, *r);
    if (FAILED(hr))
        d->set_error(E_OUTOFMEMORY);
}

void APIENTRY destroy_resource(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE hr_)
{
    Device* d = Device::from(h);
    ResourceObject* r = ResourceObject::from(hr_);
    if (r->image || r->allocated)
        destroy_shared_surface(*d, *r);
    r->~ResourceObject();
}

// --- Mapping -----------------------------------------------------------------------------------------------

void APIENTRY map(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE hr_, UINT sub, D3D10_DDI_MAP type, UINT flags,
                  D3D10DDI_MAPPED_SUBRESOURCE* out)
{
    Device* d = Device::from(h);
    *out = {};
    D3D11_MAPPED_SUBRESOURCE m{};
    // The map types and the do-not-wait flag have the API's values.
    HRESULT hr = d->ctx->Map(resource_of(hr_), sub, static_cast<D3D11_MAP>(type), flags, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        d->set_error(DXGI_DDI_ERR_WASSTILLDRAWING);
        return;
    }
    if (FAILED(hr)) {
        // Map may only report "still drawing" and device loss.
        VGD_WARN("Map(subresource %u, type %d, flags 0x%x) failed: 0x%08lx", sub, type, flags, hr);
        d->set_error(D3DDDIERR_DEVICEREMOVED);
        return;
    }
    *out = {m.pData, m.RowPitch, m.DepthPitch};
}

void APIENTRY unmap(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE hr_, UINT sub)
{
    Device::from(h)->ctx->Unmap(resource_of(hr_), sub);
}

// Whether the GPU still uses a staging resource: a map that must not wait answers it.
BOOL APIENTRY resource_is_staging_busy(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE hr_)
{
    Device* d = Device::from(h);
    ResourceObject* r = ResourceObject::from(hr_);
    D3D11_RESOURCE_DIMENSION dim;
    r->resource->GetType(&dim);
    UINT subresources = 1;
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER)
        subresources = r->mips * (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? 1 : r->array_size);
    // A map type the resource allows: staging resources have CPU read access, write access or both.
    const D3D11_MAP type = (r->cpu & D3D10_DDI_CPU_ACCESS_READ)
                               ? ((r->cpu & D3D10_DDI_CPU_ACCESS_WRITE) ? D3D11_MAP_READ_WRITE : D3D11_MAP_READ)
                               : D3D11_MAP_WRITE;
    for (UINT s = 0; s < subresources; s++) {
        D3D11_MAPPED_SUBRESOURCE m;
        HRESULT hr = d->ctx->Map(r->resource.get(), s, type, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
            return TRUE;
        if (SUCCEEDED(hr))
            d->ctx->Unmap(r->resource.get(), s);
    }
    return FALSE;
}

// --- Copies and updates ------------------------------------------------------------------------------------

void APIENTRY resource_copy(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE dst, D3D10DDI_HRESOURCE src)
{
    Device::from(h)->ctx->CopyResource(resource_of(dst), resource_of(src));
}

void APIENTRY resource_copy_region(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE dst, UINT dst_sub, UINT x, UINT y, UINT z,
                                   D3D10DDI_HRESOURCE src, UINT src_sub, const D3D10_DDI_BOX* box, UINT flags)
{
    D3D11_BOX b{};
    if (box)
        b = api_box(*box);
    // The copy flags have the API's values.
    Device::from(h)->ctx->CopySubresourceRegion1(resource_of(dst), dst_sub, x, y, z, resource_of(src), src_sub,
                                                 box ? &b : nullptr, flags);
}

void APIENTRY resource_update_subresource_up(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE dst, UINT sub,
                                             const D3D10_DDI_BOX* box, const void* data, UINT row_pitch,
                                             UINT depth_pitch, UINT flags)
{
    D3D11_BOX b{};
    if (box)
        b = api_box(*box);
    Device::from(h)->ctx->UpdateSubresource1(resource_of(dst), sub, box ? &b : nullptr, data, row_pitch,
                                             depth_pitch, flags);
}

void APIENTRY resource_resolve_subresource(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE dst, UINT dst_sub,
                                           D3D10DDI_HRESOURCE src, UINT src_sub, DXGI_FORMAT format)
{
    Device::from(h)->ctx->ResolveSubresource(resource_of(dst), dst_sub, resource_of(src), src_sub, format);
}

void APIENTRY set_resource_min_lod(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE r, FLOAT lod)
{
    Device::from(h)->ctx->SetResourceMinLOD(resource_of(r), lod);
}

// DXVK tracks hazards itself.
void APIENTRY resource_read_after_write_hazard(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE) {}

void APIENTRY discard(D3D10DDI_HDEVICE h, D3D11DDI_HANDLETYPE type, void* handle, const D3D10_DDI_RECT* rects,
                      UINT count)
{
    Device* d = Device::from(h);
    switch (type) {
    case D3D10DDI_HT_RESOURCE:
        d->ctx->DiscardResource(static_cast<ResourceObject*>(handle)->resource.get());
        break;
    case D3D10DDI_HT_SHADERRESOURCEVIEW:
    case D3D10DDI_HT_RENDERTARGETVIEW:
    case D3D10DDI_HT_DEPTHSTENCILVIEW:
    case D3D11DDI_HT_UNORDEREDACCESSVIEW:
        d->ctx->DiscardView1(static_cast<ViewObject*>(handle)->view.get(), rects, count);
        break;
    default: break;
    }
}

} // namespace

void fill_resource_ddi(D3D11_1DDI_DEVICEFUNCS& t)
{
    t.pfnCalcPrivateResourceSize = calc_private_resource_size;
    t.pfnCalcPrivateOpenedResourceSize = calc_private_opened_resource_size;
    t.pfnCreateResource = create_resource;
    t.pfnOpenResource = open_resource;
    t.pfnDestroyResource = destroy_resource;
    // One map for all: the runtime picks the entry by resource kind, the map type says the rest.
    t.pfnResourceMap = map;
    t.pfnStagingResourceMap = map;
    t.pfnDynamicIABufferMapNoOverwrite = map;
    t.pfnDynamicIABufferMapDiscard = map;
    t.pfnDynamicConstantBufferMapDiscard = map;
    t.pfnDynamicConstantBufferMapNoOverwrite = map;
    t.pfnDynamicResourceMapDiscard = map;
    t.pfnResourceUnmap = unmap;
    t.pfnStagingResourceUnmap = unmap;
    t.pfnDynamicIABufferUnmap = unmap;
    t.pfnDynamicConstantBufferUnmap = unmap;
    t.pfnDynamicResourceUnmap = unmap;
    t.pfnResourceIsStagingBusy = resource_is_staging_busy;
    t.pfnResourceCopy = resource_copy;
    t.pfnResourceConvert = resource_copy;
    t.pfnResourceCopyRegion = resource_copy_region;
    t.pfnResourceConvertRegion = resource_copy_region;
    t.pfnResourceUpdateSubresourceUP = resource_update_subresource_up;
    t.pfnDefaultConstantBufferUpdateSubresourceUP = resource_update_subresource_up;
    t.pfnResourceResolveSubresource = resource_resolve_subresource;
    t.pfnSetResourceMinLOD = set_resource_min_lod;
    t.pfnResourceReadAfterWriteHazard = resource_read_after_write_hazard;
    t.pfnDiscard = discard;
}

} // namespace vgd::umd
