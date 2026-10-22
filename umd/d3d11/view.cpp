// SPDX-License-Identifier: MIT
// Views and the clears through them. The DDI describes a view by the resource's dimension; whether it is an array
// or multisample view comes from the resource, which the API's view descriptions spell out.
#include "device.h"

#include <algorithm>

namespace vgd::umd {

ViewObject::~ViewObject()
{
    if (resource) {
        auto& v = resource->views;
        v.erase(std::remove(v.begin(), v.end(), this), v.end());
    }
}

HRESULT ViewObject::create(ID3D11Device5* d3d)
{
    ID3D11Resource* r = resource->resource.get();
    switch (kind) {
    case srv:
        return d3d->CreateShaderResourceView1(r, &desc.srv, reinterpret_cast<ID3D11ShaderResourceView1**>(view.put()));
    case rtv:
        return d3d->CreateRenderTargetView1(r, &desc.rtv, reinterpret_cast<ID3D11RenderTargetView1**>(view.put()));
    case dsv:
        return d3d->CreateDepthStencilView(r, &desc.dsv, reinterpret_cast<ID3D11DepthStencilView**>(view.put()));
    case uav:
        return d3d->CreateUnorderedAccessView1(r, &desc.uav,
                                               reinterpret_cast<ID3D11UnorderedAccessView1**>(view.put()));
    }
    return E_INVALIDARG;
}

namespace {

// Finishes a view: created on DXVK, and registered with its resource if that may have its storage rotated.
void finish(D3D10DDI_HDEVICE h, ViewObject* v, ResourceObject* r, const char* what)
{
    Device* d = Device::from(h);
    v->resource = r;
    HRESULT hr = v->create(d->d3d.get());
    if (FAILED(hr)) {
        VGD_WARN("Create%s: format %d on resource dimension %d (%u layers, %u samples) failed: 0x%08lx", what,
                 r->format, r->dimension, r->array_size, r->samples, hr);
        d->set_error(E_OUTOFMEMORY);
    }
    if (r->bind & D3D10_DDI_BIND_PRESENT)
        r->views.push_back(v);
}

template <class Args>
SIZE_T APIENTRY calc_view_size(D3D10DDI_HDEVICE, const Args*)
{
    return sizeof(ViewObject);
}

// --- Shader resource views ---------------------------------------------------------------------------------

void APIENTRY create_srv(D3D10DDI_HDEVICE h, const D3D11DDIARG_CREATESHADERRESOURCEVIEW* a,
                         D3D10DDI_HSHADERRESOURCEVIEW hv, D3D10DDI_HRTSHADERRESOURCEVIEW)
{
    auto* v = new (hv.pDrvPrivate) ViewObject(ViewObject::srv);
    ResourceObject* r = ResourceObject::from(a->hDrvResource);
    auto& d = v->desc.srv;
    d.Format = a->Format;
    const bool array = r->array_size > 1, ms = r->samples > 1;
    switch (a->ResourceDimension) {
    case D3D10DDIRESOURCE_BUFFER:
        d.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        d.Buffer = {{a->Buffer.FirstElement}, {a->Buffer.NumElements}};
        break;
    case D3D11DDIRESOURCE_BUFFEREX:
        d.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        // The raw flag has the API's value.
        d.BufferEx = {a->BufferEx.FirstElement, a->BufferEx.NumElements, a->BufferEx.Flags};
        break;
    case D3D10DDIRESOURCE_TEXTURE1D:
        if (array) {
            d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1DARRAY;
            d.Texture1DArray = {a->Tex1D.MostDetailedMip, a->Tex1D.MipLevels, a->Tex1D.FirstArraySlice,
                                a->Tex1D.ArraySize};
        } else {
            d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1D;
            d.Texture1D = {a->Tex1D.MostDetailedMip, a->Tex1D.MipLevels};
        }
        break;
    case D3D10DDIRESOURCE_TEXTURE2D:
        if (ms && array) {
            d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
            d.Texture2DMSArray = {a->Tex2D.FirstArraySlice, a->Tex2D.ArraySize};
        } else if (ms) {
            d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
        } else if (array) {
            d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray = {a->Tex2D.MostDetailedMip, a->Tex2D.MipLevels, a->Tex2D.FirstArraySlice,
                                a->Tex2D.ArraySize, 0};
        } else {
            d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            d.Texture2D = {a->Tex2D.MostDetailedMip, a->Tex2D.MipLevels, 0};
        }
        break;
    case D3D10DDIRESOURCE_TEXTURE3D:
        d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
        d.Texture3D = {a->Tex3D.MostDetailedMip, a->Tex3D.MipLevels};
        break;
    case D3D10DDIRESOURCE_TEXTURECUBE:
        if (r->array_size > 6) {
            d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
            d.TextureCubeArray = {a->TexCube.MostDetailedMip, a->TexCube.MipLevels, a->TexCube.First2DArrayFace,
                                  a->TexCube.NumCubes};
        } else {
            d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            d.TextureCube = {a->TexCube.MostDetailedMip, a->TexCube.MipLevels};
        }
        break;
    default: break;
    }
    finish(h, v, r, "ShaderResourceView");
}

void APIENTRY destroy_view(D3D10DDI_HDEVICE, void* handle)
{
    static_cast<ViewObject*>(handle)->~ViewObject();
}

void APIENTRY destroy_srv(D3D10DDI_HDEVICE h, D3D10DDI_HSHADERRESOURCEVIEW hv) { destroy_view(h, hv.pDrvPrivate); }

void APIENTRY gen_mips(D3D10DDI_HDEVICE h, D3D10DDI_HSHADERRESOURCEVIEW hv)
{
    Device::from(h)->ctx->GenerateMips(view_of<ID3D11ShaderResourceView>(hv));
}

// --- Render target views -----------------------------------------------------------------------------------

void APIENTRY create_rtv(D3D10DDI_HDEVICE h, const D3D10DDIARG_CREATERENDERTARGETVIEW* a,
                         D3D10DDI_HRENDERTARGETVIEW hv, D3D10DDI_HRTRENDERTARGETVIEW)
{
    auto* v = new (hv.pDrvPrivate) ViewObject(ViewObject::rtv);
    ResourceObject* r = ResourceObject::from(a->hDrvResource);
    auto& d = v->desc.rtv;
    d.Format = a->Format;
    const bool array = r->array_size > 1, ms = r->samples > 1;
    switch (a->ResourceDimension) {
    case D3D10DDIRESOURCE_BUFFER:
    case D3D11DDIRESOURCE_BUFFEREX:
        d.ViewDimension = D3D11_RTV_DIMENSION_BUFFER;
        d.Buffer = {{a->Buffer.FirstElement}, {a->Buffer.NumElements}};
        break;
    case D3D10DDIRESOURCE_TEXTURE1D:
        if (array) {
            d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE1DARRAY;
            d.Texture1DArray = {a->Tex1D.MipSlice, a->Tex1D.FirstArraySlice, a->Tex1D.ArraySize};
        } else {
            d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE1D;
            d.Texture1D = {a->Tex1D.MipSlice};
        }
        break;
    case D3D10DDIRESOURCE_TEXTURE2D:
        if (ms && array) {
            d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
            d.Texture2DMSArray = {a->Tex2D.FirstArraySlice, a->Tex2D.ArraySize};
        } else if (ms) {
            d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
        } else if (array) {
            d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray = {a->Tex2D.MipSlice, a->Tex2D.FirstArraySlice, a->Tex2D.ArraySize, 0};
        } else {
            d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            d.Texture2D = {a->Tex2D.MipSlice, 0};
        }
        break;
    case D3D10DDIRESOURCE_TEXTURE3D:
        d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
        d.Texture3D = {a->Tex3D.MipSlice, a->Tex3D.FirstW, a->Tex3D.WSize};
        break;
    case D3D10DDIRESOURCE_TEXTURECUBE:
        // Faces of a cube are layers of a 2D array.
        d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        d.Texture2DArray = {a->TexCube.MipSlice, a->TexCube.FirstArraySlice, a->TexCube.ArraySize, 0};
        break;
    default: break;
    }
    finish(h, v, r, "RenderTargetView");
}

void APIENTRY destroy_rtv(D3D10DDI_HDEVICE h, D3D10DDI_HRENDERTARGETVIEW hv) { destroy_view(h, hv.pDrvPrivate); }

void APIENTRY clear_rtv(D3D10DDI_HDEVICE h, D3D10DDI_HRENDERTARGETVIEW hv, FLOAT color[4])
{
    Device::from(h)->ctx->ClearRenderTargetView(view_of<ID3D11RenderTargetView>(hv), color);
}

// --- Depth/stencil views -----------------------------------------------------------------------------------

void APIENTRY create_dsv(D3D10DDI_HDEVICE h, const D3D11DDIARG_CREATEDEPTHSTENCILVIEW* a,
                         D3D10DDI_HDEPTHSTENCILVIEW hv, D3D10DDI_HRTDEPTHSTENCILVIEW)
{
    auto* v = new (hv.pDrvPrivate) ViewObject(ViewObject::dsv);
    ResourceObject* r = ResourceObject::from(a->hDrvResource);
    auto& d = v->desc.dsv;
    d.Format = a->Format;
    d.Flags = a->Flags;   // the read-only flags have the API's values
    const bool array = r->array_size > 1, ms = r->samples > 1;
    switch (a->ResourceDimension) {
    case D3D10DDIRESOURCE_TEXTURE1D:
        if (array) {
            d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE1DARRAY;
            d.Texture1DArray = {a->Tex1D.MipSlice, a->Tex1D.FirstArraySlice, a->Tex1D.ArraySize};
        } else {
            d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE1D;
            d.Texture1D = {a->Tex1D.MipSlice};
        }
        break;
    case D3D10DDIRESOURCE_TEXTURE2D:
        if (ms && array) {
            d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY;
            d.Texture2DMSArray = {a->Tex2D.FirstArraySlice, a->Tex2D.ArraySize};
        } else if (ms) {
            d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
        } else if (array) {
            d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray = {a->Tex2D.MipSlice, a->Tex2D.FirstArraySlice, a->Tex2D.ArraySize};
        } else {
            d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            d.Texture2D = {a->Tex2D.MipSlice};
        }
        break;
    case D3D10DDIRESOURCE_TEXTURECUBE:
        d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        d.Texture2DArray = {a->TexCube.MipSlice, a->TexCube.FirstArraySlice, a->TexCube.ArraySize};
        break;
    default: break;
    }
    finish(h, v, r, "DepthStencilView");
}

void APIENTRY destroy_dsv(D3D10DDI_HDEVICE h, D3D10DDI_HDEPTHSTENCILVIEW hv) { destroy_view(h, hv.pDrvPrivate); }

void APIENTRY clear_dsv(D3D10DDI_HDEVICE h, D3D10DDI_HDEPTHSTENCILVIEW hv, UINT flags, FLOAT depth, UINT8 stencil)
{
    // The clear flags have the API's values.
    Device::from(h)->ctx->ClearDepthStencilView(view_of<ID3D11DepthStencilView>(hv), flags, depth, stencil);
}

// --- Unordered access views --------------------------------------------------------------------------------

void APIENTRY create_uav(D3D10DDI_HDEVICE h, const D3D11DDIARG_CREATEUNORDEREDACCESSVIEW* a,
                         D3D11DDI_HUNORDEREDACCESSVIEW hv, D3D11DDI_HRTUNORDEREDACCESSVIEW)
{
    auto* v = new (hv.pDrvPrivate) ViewObject(ViewObject::uav);
    ResourceObject* r = ResourceObject::from(a->hDrvResource);
    auto& d = v->desc.uav;
    d.Format = a->Format;
    const bool array = r->array_size > 1;
    switch (a->ResourceDimension) {
    case D3D10DDIRESOURCE_BUFFER:
    case D3D11DDIRESOURCE_BUFFEREX:
        d.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        // The raw, append and counter flags have the API's values.
        d.Buffer = {a->Buffer.FirstElement, a->Buffer.NumElements, a->Buffer.Flags};
        break;
    case D3D10DDIRESOURCE_TEXTURE1D:
        if (array) {
            d.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE1DARRAY;
            d.Texture1DArray = {a->Tex1D.MipSlice, a->Tex1D.FirstArraySlice, a->Tex1D.ArraySize};
        } else {
            d.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE1D;
            d.Texture1D = {a->Tex1D.MipSlice};
        }
        break;
    case D3D10DDIRESOURCE_TEXTURE2D:
    case D3D10DDIRESOURCE_TEXTURECUBE:
        if (array) {
            d.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray = {a->Tex2D.MipSlice, a->Tex2D.FirstArraySlice, a->Tex2D.ArraySize, 0};
        } else {
            d.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            d.Texture2D = {a->Tex2D.MipSlice, 0};
        }
        break;
    case D3D10DDIRESOURCE_TEXTURE3D:
        d.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
        d.Texture3D = {a->Tex3D.MipSlice, a->Tex3D.FirstW, a->Tex3D.WSize};
        break;
    default: break;
    }
    finish(h, v, r, "UnorderedAccessView");
}

void APIENTRY destroy_uav(D3D10DDI_HDEVICE h, D3D11DDI_HUNORDEREDACCESSVIEW hv) { destroy_view(h, hv.pDrvPrivate); }

void APIENTRY clear_uav_uint(D3D10DDI_HDEVICE h, D3D11DDI_HUNORDEREDACCESSVIEW hv, const UINT values[4])
{
    Device::from(h)->ctx->ClearUnorderedAccessViewUint(view_of<ID3D11UnorderedAccessView>(hv), values);
}

void APIENTRY clear_uav_float(D3D10DDI_HDEVICE h, D3D11DDI_HUNORDEREDACCESSVIEW hv, const FLOAT values[4])
{
    Device::from(h)->ctx->ClearUnorderedAccessViewFloat(view_of<ID3D11UnorderedAccessView>(hv), values);
}

void APIENTRY copy_structure_count(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE dst, UINT offset,
                                   D3D11DDI_HUNORDEREDACCESSVIEW hv)
{
    Device::from(h)->ctx->CopyStructureCount(buffer_of(dst), offset, view_of<ID3D11UnorderedAccessView>(hv));
}

void APIENTRY clear_view(D3D10DDI_HDEVICE h, D3D11DDI_HANDLETYPE type, void* view, const FLOAT color[4],
                         const D3D10_DDI_RECT* rects, UINT count)
{
    if (type == D3D10DDI_HT_RENDERTARGETVIEW || type == D3D11DDI_HT_UNORDEREDACCESSVIEW)
        Device::from(h)->ctx->ClearView(static_cast<ViewObject*>(view)->view.get(), color, rects, count);
    else
        VGD_WARN("ClearView: view type %d not supported", static_cast<int>(type));
}

} // namespace

void fill_view_ddi(D3D11_1DDI_DEVICEFUNCS& t)
{
    t.pfnCalcPrivateShaderResourceViewSize = calc_view_size<D3D11DDIARG_CREATESHADERRESOURCEVIEW>;
    t.pfnCalcPrivateRenderTargetViewSize = calc_view_size<D3D10DDIARG_CREATERENDERTARGETVIEW>;
    t.pfnCalcPrivateDepthStencilViewSize = calc_view_size<D3D11DDIARG_CREATEDEPTHSTENCILVIEW>;
    t.pfnCalcPrivateUnorderedAccessViewSize = calc_view_size<D3D11DDIARG_CREATEUNORDEREDACCESSVIEW>;
    t.pfnCreateShaderResourceView = create_srv;
    t.pfnDestroyShaderResourceView = destroy_srv;
    t.pfnGenMips = gen_mips;
    t.pfnCreateRenderTargetView = create_rtv;
    t.pfnDestroyRenderTargetView = destroy_rtv;
    t.pfnClearRenderTargetView = clear_rtv;
    t.pfnCreateDepthStencilView = create_dsv;
    t.pfnDestroyDepthStencilView = destroy_dsv;
    t.pfnClearDepthStencilView = clear_dsv;
    t.pfnCreateUnorderedAccessView = create_uav;
    t.pfnDestroyUnorderedAccessView = destroy_uav;
    t.pfnClearUnorderedAccessViewUint = clear_uav_uint;
    t.pfnClearUnorderedAccessViewFloat = clear_uav_float;
    t.pfnCopyStructureCount = copy_structure_count;
    t.pfnClearView = clear_view;
}

} // namespace vgd::umd
