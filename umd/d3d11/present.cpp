// SPDX-License-Identifier: MIT
// Shared surfaces, primaries, Present and the DXGI table.
//
// Surfaces another process opens (swapchain buffers DWM composites, DWM's own primaries, DirectComposition surfaces)
// are runtime allocations whose host resource is Venus memory, as in vgd3d12 (d3d12/present.cpp):
//   1. the surface is a linear image on exported memory (linear_image.h; our ICD build exports an NT handle), which
//      DXVK wraps as a texture;
//   2. we open that handle on a kernel device of our own, which gives an allocation handle of this process;
//   3. the runtime allocation is created as an alias of it (viogpu::AllocationAlias, our KMD patch), naming the
//      same host resource.
// Opening one is the reverse: an NT handle of the opened resource goes to our ICD's dma-buf import, under a linear
// image of the same size and format. Present hands the allocations to the runtime with the present context waiting
// for our rendering on them: a fence the GPU signals through our ICD (icd/patches/0006), else a wait here.
#include "device.h"
#include "surface_format.h"

#include <atomic>

namespace vgd::umd {

namespace {

// The texture of a shared surface and the image under it: DXVK's image description for the texture, on memory that
// is exported (*fd set) or imported (fd given).
HRESULT surface_texture(Device& d, const D3D11_TEXTURE2D_DESC1& desc, int import_fd, uint64_t import_size,
                        ResourceObject& out, int* export_fd)
{
    LinearImageDesc ld;
    ld.width = desc.Width;
    ld.height = desc.Height;
    HRESULT hr = dxvk::image_info(d.d3d.get(), desc, &ld.format, &ld.view_formats, &ld.usage);
    if (FAILED(hr))
        return hr;
    if (export_fd) {
        hr = LinearImage::create_exported(d.vulkan, ld, &out.image, export_fd);
    } else {
        bool consumed = false;
        hr = LinearImage::create_imported(d.vulkan, ld, import_fd, import_size, &out.image, &consumed);
        if (!consumed)
            CloseHandle(handle_of_fd(import_fd));
    }
    if (FAILED(hr))
        return hr;
    Com<ID3D11Texture2D> tex;
    hr = dxvk::texture_from_image(d.d3d.get(), desc, out.image->image(), tex.put());
    if (FAILED(hr)) {
        VGD_WARN("shared surface: DXVK did not take the image: 0x%08lx", hr);
        return hr;
    }
    out.resource = tex.as<ID3D11Resource>();
    return S_OK;
}

D3D11_TEXTURE2D_DESC1 surface_desc(DXGI_FORMAT format, UINT width, UINT height, UINT bind)
{
    return {width, height, 1, 1, format, {1, 0}, D3D11_USAGE_DEFAULT, bind, 0, 0, D3D11_TEXTURE_LAYOUT_UNDEFINED};
}

} // namespace

bool is_shared_surface(const D3D11DDIARG_CREATERESOURCE& a)
{
    // Presentable surfaces need runtime allocations: Present hands the allocations over.
    return a.pPrimaryDesc || (a.MiscFlags & D3D10_DDI_RESOURCE_MISC_SHARED) || (a.BindFlags & D3D10_DDI_BIND_PRESENT);
}

HRESULT create_shared_surface(Device& d, const D3D11DDIARG_CREATERESOURCE& a, D3D10DDI_HRTRESOURCE rt,
                              ResourceObject& out)
{
    const UINT width = a.pMipInfoList[0].TexelWidth, height = a.pMipInfoList[0].TexelHeight;
    if (a.ResourceDimension != D3D10DDIRESOURCE_TEXTURE2D || a.MipLevels != 1 || a.ArraySize != 1 ||
        a.SampleDesc.Count != 1 || (a.BindFlags & D3D10_DDI_BIND_DEPTH_STENCIL)) {
        VGD_WARN("shared surface: only single 2D colour textures are supported (dimension %d, %u mips, %u layers, "
                 "%u samples, bind 0x%x)",
                 a.ResourceDimension, a.MipLevels, a.ArraySize, a.SampleDesc.Count, a.BindFlags);
        return E_NOTIMPL;
    }
    if (!d.allocation_alias) {
        VGD_WARN("shared surface: the KMD cannot alias allocations (yttrium's unpatched viogpu3d?)");
        return E_NOTIMPL;
    }
    const ULONG format = virgl_format(a.Format);
    if (!format && a.pPrimaryDesc) {
        VGD_WARN("shared surface: primary of format %d cannot be scanned out", a.Format);
        return E_NOTIMPL;
    }
    // Openers sample it and may render to it; DWM's primaries are render targets.
    const UINT bind = api_bind_flags(a.BindFlags) | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    const D3D11_TEXTURE2D_DESC1 desc = surface_desc(a.Format, width, height, bind);
    int fd = -1;
    HRESULT hr = surface_texture(d, desc, -1, 0, out, &fd);
    if (FAILED(hr)) {
        VGD_WARN("shared surface: %ux%u format %d failed: 0x%08lx", width, height, a.Format, hr);
        return hr;
    }
    const HANDLE nt = handle_of_fd(fd);
    KmtDevice* kmt = d.kernel();
    D3DKMT_HANDLE opened = 0, source = 0;
    hr = kmt ? kmt->open_shared(nt, &opened, &source) : E_FAIL;
    CloseHandle(nt);   // the opened resource keeps the allocation
    if (FAILED(hr))
        return hr;

    viogpu::AllocationAlias alias{};
    auto& o = alias.exchange.options;
    o.target = viogpu::kPipeTexture2d;
    o.format = format;
    o.bind = viogpu::kBindRenderTarget | viogpu::kBindSamplerView | viogpu::kBindShared;
    if (a.pPrimaryDesc || (a.BindFlags & D3D10_DDI_BIND_PRESENT))
        o.bind |= viogpu::kBindDisplayTarget;
    o.width = width;
    o.height = height;
    o.depth = 1;
    o.array_size = 1;
    alias.exchange.Size = out.image->size();
    alias.exchange.BlobMem = viogpu::kBlobMemHost3d;
    alias.exchange.BlobFlags = viogpu::kBlobFlagShareable;
    alias.exchange.Stride = out.image->row_pitch();
    alias.Magic = viogpu::kAllocationAliasMagic;
    alias.SourceAllocation = source;
    SharedSurfaceInfo surface;
    surface.format = a.Format;
    surface.width = width;
    surface.height = height;
    surface.bind = bind;

    D3DDDI_ALLOCATIONINFO info{};
    info.pPrivateDriverData = &alias;
    info.PrivateDriverDataSize = sizeof(alias);
    if (a.pPrimaryDesc) {
        info.VidPnSourceId = a.pPrimaryDesc->VidPnSourceId;
        info.Flags.Primary = 1;
    }
    D3DDDICB_ALLOCATE cb{};
    cb.pPrivateDriverData = &surface;
    cb.PrivateDriverDataSize = sizeof(surface);
    cb.hResource = rt.handle;
    cb.NumAllocations = 1;
    cb.pAllocationInfo = &info;
    const UINT host_res = kmt->resource_id(source);   // for the log: the host names it in its errors
    hr = d.kt.pfnAllocateCb(d.rt_device.handle, &cb);
    kmt->close(opened);
    if (FAILED(hr) || !info.hAllocation) {
        VGD_WARN("shared surface: AllocateCb failed: 0x%08lx", hr);
        return FAILED(hr) ? hr : E_FAIL;
    }
    out.allocation = info.hAllocation;
    out.allocated = true;
    VGD_TRACE("shared surface: %ux%u format %d%s, allocation 0x%x (km resource 0x%x) aliases 0x%x (host resource "
              "%u): %llu bytes, pitch %u",
              width, height, a.Format, a.pPrimaryDesc ? " primary" : "", info.hAllocation, cb.hKMResource, source,
              host_res, static_cast<unsigned long long>(out.image->size()), out.image->row_pitch());
    return S_OK;
}

HRESULT open_shared_surface(Device& d, const D3D10DDIARG_OPENRESOURCE& a, ResourceObject& out)
{
    if (a.NumAllocations != 1 || !a.pOpenAllocationInfo ||
        a.pOpenAllocationInfo[0].PrivateDriverDataSize < sizeof(viogpu::AllocationExchange)) {
        VGD_WARN("OpenResource: %u allocations, private data %u bytes: not a surface of ours", a.NumAllocations,
                 a.pOpenAllocationInfo ? a.pOpenAllocationInfo[0].PrivateDriverDataSize : 0);
        return E_NOTIMPL;
    }
    const auto& e = *static_cast<const viogpu::AllocationExchange*>(a.pOpenAllocationInfo[0].pPrivateDriverData);
    const viogpu::ResourceOptions& o = e.options;
    DXGI_FORMAT srgb;
    DXGI_FORMAT format = dxgi_format(o.format, &srgb);
    UINT width = o.width, height = o.height;
    UINT bind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    const auto* surface = static_cast<const SharedSurfaceInfo*>(a.pPrivateDriverData);
    const bool ours = surface && a.PrivateDriverDataSize == sizeof(SharedSurfaceInfo) &&
                      surface->magic == SharedSurfaceInfo::kMagic;
    if (ours) {
        format = static_cast<DXGI_FORMAT>(surface->format);
        width = surface->width;
        height = surface->height;
        bind = surface->bind;
    }
    VGD_TRACE("OpenResource: allocation 0x%x, km resource 0x%x: %s format %d %ux%u bind 0x%x; virgl format %lu, "
              "size %llu, blob mem %lu, blob flags 0x%lx, stride %lu",
              a.pOpenAllocationInfo[0].hAllocation, a.hKMResource.handle, ours ? "ours," : "foreign,", format, width,
              height, bind, o.format, static_cast<unsigned long long>(e.Size), e.BlobMem, e.BlobFlags, e.Stride);
    if (o.target != viogpu::kPipeTexture2d || e.BlobMem != viogpu::kBlobMemHost3d ||
        !(e.BlobFlags & viogpu::kBlobFlagShareable) || format == DXGI_FORMAT_UNKNOWN) {
        VGD_WARN("OpenResource: only shareable 2D blob surfaces of known formats can be opened");
        return E_NOTIMPL;
    }
    KmtDevice* kmt = d.kernel();
    HANDLE nt = nullptr;
    if (!kmt || FAILED(kmt->share(a.hKMResource.handle, &nt)))
        return E_FAIL;
    // For the log: the host names resources by this id in its errors (an import of one it does not know kills the
    // Venus context).
    D3DKMT_HANDLE opened = 0, source = 0;
    if (SUCCEEDED(kmt->open_shared(nt, &opened, &source))) {
        VGD_TRACE("OpenResource: km resource 0x%x is host resource %u", a.hKMResource.handle,
                  kmt->resource_id(source));
        kmt->close(opened);
    }
    const int fd = fd_of_handle(nt);
    if (fd < 0) {
        CloseHandle(nt);
        return E_FAIL;
    }
    out.dimension = D3D10DDIRESOURCE_TEXTURE2D;
    out.format = format;
    out.width = width;
    out.height = height;
    HRESULT hr = surface_texture(d, surface_desc(format, width, height, bind), fd, e.Size, out, nullptr);
    if (FAILED(hr))
        VGD_WARN("OpenResource: import of %ux%u format %d failed: 0x%08lx", width, height, format, hr);
    return hr;
}

void destroy_shared_surface(Device& d, ResourceObject& r)
{
    // The image goes with the texture: the GPU must be done with it first.
    if (r.image)
        d.wait_idle();
    r.resource = nullptr;
    r.image.reset();
    if (r.allocated) {
        // By resource, which frees its allocations.
        D3DDDICB_DEALLOCATE cb{};
        cb.hResource = r.rt_resource.handle;
        HRESULT hr = d.kt.pfnDeallocateCb(d.rt_device.handle, &cb);
        if (FAILED(hr))
            VGD_WARN("shared surface: DeallocateCb(0x%x) failed: 0x%08lx", r.allocation, hr);
        r.allocated = false;
    }
}

namespace {

// Our ICD's entry points (icd/patches/0006), through vkGetDeviceProcAddr.
using PFN_vkOpenFenceVGD = VkResult(VKAPI_PTR*)(VkDevice device, HANDLE nt_handle, uint32_t* fence);
using PFN_vkCloseFenceVGD = void(VKAPI_PTR*)(VkDevice device, uint32_t fence);
using PFN_vkQueueSignalFenceVGD = VkResult(VKAPI_PTR*)(VkQueue queue, uint32_t fence, uint64_t value);

// Device::render_fence, on the first present: a shared monitored fence of the runtime's, which our ICD opens on its
// own kernel device from an NT handle. Not set up (logged) if the ICD or the KMD cannot.
void init_render_fence(Device& d)
{
    auto& f = d.render_fence;
    f.tried = true;
    char off[4] = {};
    if (setting("VGD_PRESENT_GPU_WAIT", off, sizeof(off)) && off[0] == '0') {
        VGD_TRACE("Present: VGD_PRESENT_GPU_WAIT=0: presents wait for the GPU here");
        return;
    }
    const auto& vk = d.vulkan.vk;
    auto open = reinterpret_cast<PFN_vkOpenFenceVGD>(vk.vkGetDeviceProcAddr(d.vulkan.device, "vkOpenFenceVGD"));
    f.close = vk.vkGetDeviceProcAddr(d.vulkan.device, "vkCloseFenceVGD");
    f.signal = vk.vkGetDeviceProcAddr(d.vulkan.device, "vkQueueSignalFenceVGD");
    KmtDevice* kmt = d.kernel();
    if (!open || !f.close || !f.signal || !kmt || !d.kt.pfnCreateSynchronizationObject2Cb ||
        !d.kt.pfnWaitForSynchronizationObjectFromGpuCb) {
        VGD_WARN("Present: no GPU fence (ICD entry points %d, kernel device %d): presents wait for the GPU here",
                 open && f.close && f.signal, kmt != nullptr);
        return;
    }
    D3DDDICB_CREATESYNCHRONIZATIONOBJECT2 c{};
    c.Info.Type = D3DDDI_MONITORED_FENCE;
    c.Info.Flags.Shared = 1;
    c.Info.Flags.NtSecuritySharing = 1;
    HRESULT hr = d.kt.pfnCreateSynchronizationObject2Cb(d.rt_device.handle, &c);
    if (FAILED(hr)) {
        VGD_WARN("Present: CreateSynchronizationObject2Cb (shared monitored fence) failed: 0x%08lx", hr);
        return;
    }
    HANDLE nt = nullptr;
    uint32_t icd = 0;
    hr = kmt->share(c.hSyncObject, D3DDDI_SYNC_OBJECT_ALL_ACCESS, &nt);
    if (SUCCEEDED(hr)) {
        const VkResult r = open(d.vulkan.device, nt, &icd);
        CloseHandle(nt);
        if (r != VK_SUCCESS) {
            VGD_WARN("Present: vkOpenFenceVGD failed: %d", r);
            hr = E_FAIL;
        }
    }
    if (FAILED(hr)) {
        D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT dc{c.hSyncObject};
        if (FAILED(d.kt.pfnDestroySynchronizationObjectCb(d.rt_device.handle, &dc)))
            VGD_WARN("Present: DestroySynchronizationObjectCb failed");
        return;
    }
    f.sync = c.hSyncObject;
    f.icd = icd;
    VGD_TRACE("Present: GPU fence 0x%x (ICD 0x%x): presents wait for our rendering on the GPU", f.sync, f.icd);
}

struct SignalArgs {
    PFN_vkQueueSignalFenceVGD signal;
    uint32_t fence;
    uint64_t value;
};

HRESULT signal_on_queue(VkQueue queue, void* data)
{
    const auto* a = static_cast<const SignalArgs*>(data);
    const VkResult r = a->signal(queue, a->fence, a->value);
    if (r == VK_NOT_READY)
        return S_FALSE;   // nothing submitted yet
    return r == VK_SUCCESS ? S_OK : E_FAIL;
}

// Queues a wait on the present context for all our rendering so far; S_FALSE if there is nothing to wait for, a
// failure if the fence cannot be used (then the caller waits for the GPU itself).
HRESULT wait_for_rendering_on_gpu(Device& d)
{
    auto& f = d.render_fence;
    if (!f.tried)
        init_render_fence(d);
    if (!f.sync)
        return E_NOTIMPL;
    SignalArgs a{reinterpret_cast<PFN_vkQueueSignalFenceVGD>(f.signal), f.icd, f.value + 1};
    HRESULT hr = dxvk::with_submitted_queue(d.d3d.get(), signal_on_queue, &a);
    if (hr != S_OK) {
        if (FAILED(hr))
            VGD_WARN("Present: vkQueueSignalFenceVGD failed: 0x%08lx", hr);
        return hr;
    }
    f.value = a.value;
    D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU w{};
    w.hContext = d.present_context;
    w.ObjectCount = 1;
    w.ObjectHandleArray = &f.sync;
    w.MonitoredFenceValueArray = &f.value;
    hr = d.kt.pfnWaitForSynchronizationObjectFromGpuCb(d.rt_device.handle, &w);
    if (FAILED(hr))
        VGD_WARN("Present: WaitForSynchronizationObjectFromGpuCb(%llu) failed: 0x%08lx",
                 static_cast<unsigned long long>(f.value), hr);
    return hr;
}

} // namespace

void destroy_render_fence(Device& d)
{
    auto& f = d.render_fence;
    if (!f.sync)
        return;
    reinterpret_cast<PFN_vkCloseFenceVGD>(f.close)(d.vulkan.device, f.icd);
    D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT dc{f.sync};
    if (FAILED(d.kt.pfnDestroySynchronizationObjectCb(d.rt_device.handle, &dc)))
        VGD_WARN("DestroyDevice: DestroySynchronizationObjectCb failed");
    f = {};
}

namespace {

// --- Present -------------------------------------------------------------------------------------------------

HRESULT present_allocations(Device& d, D3DKMT_HANDLE src, D3DKMT_HANDLE dst, void* dxgi_context)
{
    if (!d.present_context) {
        // A physical (non-virtual) context: yttrium's KMD rejects virtual application contexts. Nothing is
        // submitted to it; presents are queued on it.
        D3DDDICB_CREATECONTEXT cc{};
        cc.NodeOrdinal = 0;
        cc.EngineAffinity = 1;
        HRESULT hr = d.kt.pfnCreateContextCb(d.rt_device.handle, &cc);
        if (FAILED(hr) || !cc.hContext) {
            VGD_WARN("Present: CreateContextCb failed: 0x%08lx", hr);
            return E_OUTOFMEMORY;
        }
        d.present_context = cc.hContext;
    }
    // The runtime presents the allocations as they are: our rendering on them must be complete. The present
    // context waits for it on the GPU where it can, else we wait here.
    HRESULT hr = wait_for_rendering_on_gpu(d);
    if (FAILED(hr))
        hr = d.wait_idle();
    if (FAILED(hr))
        return hr;
    DXGIDDICB_PRESENT p{};
    p.hSrcAllocation = src;
    p.hDstAllocation = dst;
    p.pDXGIContext = dxgi_context;
    p.hContext = d.present_context;
    return d.dxgi->pfnPresentCb(d.rt_device.handle, &p);
}

D3DKMT_HANDLE allocation_of(DXGI_DDI_HRESOURCE h)
{
    return h ? ResourceObject::from(h)->allocation : 0;
}

HRESULT APIENTRY present(DXGI_DDI_ARG_PRESENT* a)
{
    Device& d = *Device::from(a->hDevice);
    const D3DKMT_HANDLE src = allocation_of(a->hSurfaceToPresent), dst = allocation_of(a->hDstResource);
    static std::atomic<unsigned> count{0};
    const unsigned n = count.fetch_add(1, std::memory_order_relaxed);
    if (!src) {
        VGD_WARN("Present: surface %p has no allocation", reinterpret_cast<void*>(a->hSurfaceToPresent));
        return E_INVALIDARG;
    }
    // Also before: a present that blocks (on our rendering or in the KMD) never reaches the line below.
    if (n < 8 || verbose())
        VGD_TRACE("Present #%u: src 0x%x, dst 0x%x: begin", n, src, dst);
    HRESULT hr = present_allocations(d, src, dst, a->pDXGIContext);
    if (n < 8 || FAILED(hr) || verbose())
        VGD_TRACE("Present #%u: src 0x%x (sub %u), dst 0x%x (sub %u), flags 0x%x, interval %d -> 0x%08lx", n, src,
                  a->SrcSubResourceIndex, dst, a->DstSubResourceIndex, a->Flags.Value,
                  static_cast<int>(a->FlipInterval), hr);
    return hr;
}

// A copy between surfaces of one size and format family; stretching, conversion and rotation are not supported.
HRESULT blt(Device& d, DXGI_DDI_HRESOURCE dst, UINT dst_sub, UINT dst_left, UINT dst_top, DXGI_DDI_HRESOURCE src,
            UINT src_sub, const D3D11_BOX* src_box, DXGI_DDI_ARG_BLT_FLAGS flags, DXGI_DDI_MODE_ROTATION rotate)
{
    ResourceObject* s = ResourceObject::from(src);
    ResourceObject* t = ResourceObject::from(dst);
    if (flags.Stretch || flags.Convert || (rotate != DXGI_DDI_MODE_ROTATION_UNSPECIFIED &&
                                           rotate != DXGI_DDI_MODE_ROTATION_IDENTITY)) {
        VGD_WARN("Blt: flags 0x%x, rotation %d: stretching, conversion and rotation not supported; copying",
                 flags.Value, rotate);
    }
    if (flags.Resolve && s->samples > 1)
        d.ctx->ResolveSubresource(t->resource.get(), dst_sub, s->resource.get(), src_sub, t->format);
    else
        d.ctx->CopySubresourceRegion(t->resource.get(), dst_sub, dst_left, dst_top, 0, s->resource.get(), src_sub,
                                     src_box);
    if (flags.Present)
        return d.wait_idle();
    return S_OK;
}

HRESULT APIENTRY blt_ddi(DXGI_DDI_ARG_BLT* a)
{
    return blt(*Device::from(a->hDevice), a->hDstResource, a->DstSubresource, a->DstLeft, a->DstTop, a->hSrcResource,
               a->SrcSubresource, nullptr, a->Flags, a->Rotate);
}

HRESULT APIENTRY blt1(DXGI_DDI_ARG_BLT1* a)
{
    const D3D11_BOX box{a->SrcLeft, a->SrcTop, 0, a->SrcRight, a->SrcBottom, 1};
    const bool whole = a->SrcRight <= a->SrcLeft || a->SrcBottom <= a->SrcTop;
    return blt(*Device::from(a->hDevice), a->hDstResource, a->DstSubresource, a->DstLeft, a->DstTop, a->hSrcResource,
               a->SrcSubresource, whole ? nullptr : &box, a->Flags, a->Rotate);
}

// Resource 0 takes the storage of 1, 1 that of 2, ..., the last that of 0; views follow the storage.
HRESULT APIENTRY rotate_resource_identities(DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES* a)
{
    Device& d = *Device::from(a->hDevice);
    const UINT n = a->Resources;
    if (n < 2)
        return S_OK;
    std::vector<ResourceObject*> r(n);
    for (UINT i = 0; i < n; i++)
        r[i] = ResourceObject::from(a->pResources[i]);
    Com<ID3D11Resource> first_resource = std::move(r[0]->resource);
    std::unique_ptr<LinearImage> first_image = std::move(r[0]->image);
    const D3DKMT_HANDLE first_allocation = r[0]->allocation;
    for (UINT i = 0; i + 1 < n; i++) {
        r[i]->resource = std::move(r[i + 1]->resource);
        r[i]->image = std::move(r[i + 1]->image);
        r[i]->allocation = r[i + 1]->allocation;
    }
    r[n - 1]->resource = std::move(first_resource);
    r[n - 1]->image = std::move(first_image);
    r[n - 1]->allocation = first_allocation;
    // Views are made again over the new storage; bound render targets are rebound.
    bool rebind = false;
    for (ResourceObject* res : r)
        for (ViewObject* v : res->views) {
            ID3D11View* old = v->view.get();
            if (FAILED(v->create(d.d3d.get())))
                VGD_WARN("RotateResourceIdentities: view %p could not be made again", static_cast<void*>(v));
            for (auto& rtv : d.om_rtvs)
                if (rtv == old) {
                    rtv = v->as<ID3D11RenderTargetView>();
                    rebind = true;
                }
            if (d.om_dsv == old) {
                d.om_dsv = v->as<ID3D11DepthStencilView>();
                rebind = true;
            }
        }
    if (rebind)
        d.ctx->OMSetRenderTargets(static_cast<UINT>(d.om_rtvs.size()), d.om_rtvs.data(), d.om_dsv);
    return S_OK;
}

HRESULT APIENTRY set_display_mode(DXGI_DDI_ARG_SETDISPLAYMODE* a)
{
    Device& d = *Device::from(a->hDevice);
    D3DDDICB_SETDISPLAYMODE m{};
    m.hPrimaryAllocation = allocation_of(a->hResource);
    HRESULT hr = d.kt.pfnSetDisplayModeCb(d.rt_device.handle, &m);
    VGD_TRACE("SetDisplayMode: allocation 0x%x -> 0x%08lx", m.hPrimaryAllocation, hr);
    return hr;
}

HRESULT APIENTRY get_gamma_caps(DXGI_DDI_ARG_GET_GAMMA_CONTROL_CAPS* a)
{
    if (a && a->pGammaCapabilities)
        *a->pGammaCapabilities = {};
    return S_OK;
}

HRESULT APIENTRY set_resource_priority(DXGI_DDI_ARG_SETRESOURCEPRIORITY*) { return S_OK; }

HRESULT APIENTRY query_resource_residency(DXGI_DDI_ARG_QUERYRESOURCERESIDENCY* a)
{
    for (SIZE_T i = 0; a && a->pStatus && i < a->Resources; i++)
        a->pStatus[i] = DXGI_DDI_RESIDENCY_FULLY_RESIDENT;
    return S_OK;
}

HRESULT APIENTRY resolve_shared_resource(DXGI_DDI_ARG_RESOLVESHAREDRESOURCE*) { return S_OK; }

// Nothing is ever discarded: offered resources keep their memory.
HRESULT APIENTRY offer_resources(DXGI_DDI_ARG_OFFERRESOURCES*) { return S_OK; }

HRESULT APIENTRY reclaim_resources(DXGI_DDI_ARG_RECLAIMRESOURCES* a)
{
    for (UINT i = 0; a && a->pDiscarded && i < a->Resources; i++)
        a->pDiscarded[i] = FALSE;
    return S_OK;
}

} // namespace

void fill_dxgi_ddi(DXGI1_2_DDI_BASE_FUNCTIONS& t)
{
    t.pfnPresent = present;
    t.pfnGetGammaCaps = get_gamma_caps;
    t.pfnSetDisplayMode = set_display_mode;
    t.pfnSetResourcePriority = set_resource_priority;
    t.pfnQueryResourceResidency = query_resource_residency;
    t.pfnRotateResourceIdentities = rotate_resource_identities;
    t.pfnBlt = blt_ddi;
    t.pfnResolveSharedResource = resolve_shared_resource;
    t.pfnBlt1 = blt1;
    t.pfnOfferResources = offer_resources;
    t.pfnReclaimResources = reclaim_resources;
    // Multiplane overlays: the runtime only uses these from interface builds after the D3D11.1 one we offer.
    t.pfnGetMultiplaneOverlayCaps = nullptr;
    t.pfnGetMultiplaneOverlayFilterRange = nullptr;
    t.pfnCheckMultiplaneOverlaySupport = nullptr;
    t.pfnPresentMultiplaneOverlay = nullptr;
}

} // namespace vgd::umd
