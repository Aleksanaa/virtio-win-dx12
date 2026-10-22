// SPDX-License-Identifier: MIT
// Swapchain buffers, Present and the DXGI table.
//
// DXGI creates swapchain buffers as committed resources on PRIMARY heaps, and the compositor (DWM, through
// yttrium's D3D10 UMD) opens their runtime allocation and imports its host resource into its own Venus context.
// That resource must be a dma-buf exported from Vulkan memory, and the stock KMD creates a blob only in the context
// of the device that first opens the allocation, here the D3D12 runtime's device, which has no Venus context. So:
//   1. the buffer is a linear texture on exported memory (shared_image.h; our ICD build exports an NT handle);
//   2. we open that handle on a kernel device of our own, which gives an allocation handle of this process;
//   3. the runtime allocation is created as an alias of it (viogpu::AllocationAlias, our KMD patch): it names the
//      same host resource, which lives until both are gone.
// Present is then zero-copy: the buffer's allocation goes to the runtime, which presents it on the queue's
// context, where our queue bridge has ordered it after the rendering.
#include "device.h"
#include "log.h"
#include "objects.h"
#include "kmt.h"
#include "surface_format.h"
#include "viogpu_abi.h"

#include <atomic>
#include <memory>
#include <new>
#include <vector>

namespace vgd::umd {

namespace {

// Our kernel device (kmt.h), created on first use.
KmtDevice* kmt_device(Device& d)
{
    if (!d.kmt)
        d.kmt = KmtDevice::open();
    return d.kmt.get();
}

} // namespace

// --- Swapchain buffers ---------------------------------------------------------------------------------------

HRESULT create_primary(Device& d, const D3D12DDIARG_CREATEHEAP_0001& heap, const D3D12DDIARG_CREATERESOURCE_0088& args,
                       const D3D12_RESOURCE_DESC& desc, D3D12DDI_HRTRESOURCE rt_resource, HeapObject& out)
{
    VGD_TRACE("CreateHeapAndResource: swapchain buffer %llux%u format %d, heap flags 0x%x, resource flags 0x%x, "
              "%u castable formats",
              static_cast<unsigned long long>(desc.Width), desc.Height, static_cast<int>(desc.Format),
              static_cast<unsigned>(heap.Flags), static_cast<unsigned>(args.Flags), args.NumCastableFormats);
    if (!d.allocation_alias) {
        VGD_WARN("present: the KMD cannot alias allocations (yttrium's unpatched viogpu3d?): no swapchains");
        return E_OUTOFMEMORY;
    }
    const ULONG format = virgl_format(desc.Format);
    if (!format) {
        VGD_WARN("present: swapchain format %d is not supported yet", static_cast<int>(desc.Format));
        return E_OUTOFMEMORY;
    }

    int fd = -1;
    HRESULT hr = SharedImage::create_exported(d.engine, desc, args.pCastableFormats, args.NumCastableFormats,
                                              &out.shared, &fd);
    if (FAILED(hr)) {
        VGD_WARN("present: presentable texture failed: 0x%08lx", hr);
        return E_OUTOFMEMORY;
    }
    const HANDLE nt = handle_of_fd(fd);
    KmtDevice* kmt = kmt_device(d);
    D3DKMT_HANDLE opened_resource = 0, source = 0;
    if (kmt && nt != INVALID_HANDLE_VALUE)
        hr = kmt->open_shared(nt, &opened_resource, &source);
    else
        hr = E_FAIL;
    if (nt != INVALID_HANDLE_VALUE)
        CloseHandle(nt);   // the opened resource keeps the allocation
    if (FAILED(hr)) {
        VGD_WARN("present: could not open the exported memory (fd %d, handle %p)", fd, nt);
        out.shared.reset();
        return E_OUTOFMEMORY;
    }

    viogpu::AllocationAlias alias{};
    auto& o = alias.exchange.options;
    o.target = viogpu::kPipeTexture2d;
    o.format = format;
    o.bind = viogpu::kBindRenderTarget | viogpu::kBindSamplerView | viogpu::kBindDisplayTarget | viogpu::kBindShared;
    o.width = static_cast<ULONG>(desc.Width);
    o.height = desc.Height;
    o.depth = 1;
    o.array_size = 1;
    alias.exchange.Size = out.shared->size();
    alias.exchange.BlobMem = viogpu::kBlobMemHost3d;
    alias.exchange.BlobFlags = viogpu::kBlobFlagShareable;
    alias.exchange.Stride = out.shared->row_pitch();
    alias.Magic = viogpu::kAllocationAliasMagic;
    alias.SourceAllocation = source;
    SharedSurfaceInfo surface;   // what the surface is, for our D3D11 UMD opening it (in DWM)
    surface.format = desc.Format;
    surface.width = static_cast<UINT>(desc.Width);
    surface.height = desc.Height;
    surface.bind = SharedSurfaceInfo::kBindShaderResource | SharedSurfaceInfo::kBindRenderTarget;

    D3D12DDI_ALLOCATION_INFO_0022 info{};
    info.pPrivateDriverData = &alias;
    info.PrivateDriverDataSize = sizeof(alias);
    info.Flags = D3D12DDI_ALLOCATION_INFO_FLAGS_0022_PRIMARY;
    D3D12DDICB_ALLOCATE_0022 cb{};
    cb.pPrivateDriverData = &surface;
    cb.PrivateDriverDataSize = sizeof(surface);
    cb.hResource = rt_resource.handle;
    cb.NumAllocations = 1;
    cb.pAllocationInfo = &info;
    hr = d.um_callbacks->pfnAllocateCb(d.rt_device, &cb);
    if (FAILED(hr)) {
        VGD_WARN("present: AllocateCb (primary) failed: 0x%08lx; retrying as a plain allocation", hr);
        info = {};
        info.pPrivateDriverData = &alias;
        info.PrivateDriverDataSize = sizeof(alias);
        cb.hKMResource = 0;
        hr = d.um_callbacks->pfnAllocateCb(d.rt_device, &cb);
    }
    kmt->close(opened_resource);
    if (FAILED(hr) || !info.hAllocation) {
        VGD_WARN("present: AllocateCb failed: 0x%08lx", hr);
        out.shared.reset();
        return E_OUTOFMEMORY;
    }
    out.allocation = info.hAllocation;
    out.rt_resource = rt_resource;
    VGD_TRACE("present: swapchain buffer allocation 0x%x (km resource 0x%x) aliases 0x%x: %llu bytes, pitch %u, "
              "virgl format %lu",
              info.hAllocation, cb.hKMResource, source, static_cast<unsigned long long>(out.shared->size()),
              out.shared->row_pitch(), format);
    return S_OK;
}

void destroy_primary(Device& d, HeapObject& heap)
{
    // By resource (which frees its allocations) or by allocation list, never both: both is E_INVALIDARG.
    D3D12DDICB_DEALLOCATE_0022 cb{};
    cb.hResource = heap.rt_resource.handle;
    cb.Flags = D3D12DDI_DEALLOCATE_FLAGS_0022_ASSUME_NOT_IN_USE | D3D12DDI_DEALLOCATE_FLAGS_0022_SYNCHRONOUS_DESTROY;
    HRESULT hr = d.um_callbacks->pfnDeallocateCb(d.rt_device, &cb);
    if (FAILED(hr))
        VGD_WARN("present: DeallocateCb(0x%x) failed: 0x%08lx", heap.allocation, hr);
    heap.allocation = 0;
}

// --- Textures other drivers share ----------------------------------------------------------------------------
// ID3D12Device::OpenSharedHandle on a texture yttrium's D3D10/11 UMD created shared (a browser compositor's canvas,
// for WebGPU): the runtime has opened its allocation, whose private data is yttrium's allocation exchange. Its
// memory is Venus memory exported as a shareable blob, under a linear image (yttrium_venus2_resource.c), so it is
// imported like the reverse of a swapchain buffer: an NT handle of the opened resource goes to our ICD's dma-buf
// import (icd/patches/0003, 0005), under a linear image of the same size and format (shared_image.h).

HRESULT open_shared(Device& d, const D3D12DDIARG_OPENHEAP_0003& args, HeapObject& out)
{
    if (args.NumAllocations != 1 || !args.pOpenAllocationInfo ||
        args.pOpenAllocationInfo[0].PrivateDriverDataSize < sizeof(viogpu::AllocationExchange)) {
        VGD_WARN("OpenHeapAndResource: %u allocations, private data %u bytes: not one of yttrium's allocations",
                 args.NumAllocations,
                 args.pOpenAllocationInfo ? args.pOpenAllocationInfo[0].PrivateDriverDataSize : 0);
        return E_OUTOFMEMORY;
    }
    const auto& e = *static_cast<const viogpu::AllocationExchange*>(args.pOpenAllocationInfo[0].pPrivateDriverData);
    const viogpu::ResourceOptions& o = e.options;
    VGD_TRACE("OpenHeapAndResource: allocation 0x%x, km resource 0x%x: target %lu format %lu bind 0x%lx %lux%lux%lu "
              "array %lu levels %lu samples %lu; size %llu, blob mem %lu, blob flags 0x%lx, stride %lu",
              args.pOpenAllocationInfo[0].hAllocation, static_cast<unsigned>(args.hKMResource.handle), o.target,
              o.format, o.bind, o.width, o.height, o.depth, o.array_size, o.last_level + 1, o.nr_samples,
              static_cast<unsigned long long>(e.Size), e.BlobMem, e.BlobFlags, e.Stride);
    DXGI_FORMAT srgb;
    const DXGI_FORMAT format = dxgi_format(o.format, &srgb);
    if (o.target != viogpu::kPipeTexture2d || o.depth > 1 || o.array_size > 1 || o.last_level || o.nr_samples > 1 ||
        e.BlobMem != viogpu::kBlobMemHost3d || !(e.BlobFlags & viogpu::kBlobFlagShareable) ||
        format == DXGI_FORMAT_UNKNOWN) {
        VGD_WARN("OpenHeapAndResource: only shareable single 2D textures of swapchain formats so far");
        return E_OUTOFMEMORY;
    }
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = o.width;
    desc.Height = o.height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc = {1, 0};
    if (o.bind & viogpu::kBindRenderTarget)
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (o.bind & viogpu::kBindShaderImage)
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (!(o.bind & viogpu::kBindSamplerView))
        desc.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    KmtDevice* kmt = kmt_device(d);
    if (!kmt)
        return E_OUTOFMEMORY;
    HANDLE nt = nullptr;
    if (FAILED(kmt->share(args.hKMResource.handle, &nt)))
        return E_OUTOFMEMORY;
    const int fd = fd_of_handle(nt);
    if (fd < 0) {
        CloseHandle(nt);
        return E_OUTOFMEMORY;
    }
    bool consumed = false;
    HRESULT hr = SharedImage::create_imported(d.engine, desc, &srgb, srgb != DXGI_FORMAT_UNKNOWN ? 1 : 0, fd, e.Size,
                                              &out.shared, &consumed);
    if (!consumed)
        CloseHandle(nt);
    if (FAILED(hr)) {
        VGD_WARN("OpenHeapAndResource: import of %lux%lu format %d failed: 0x%08lx", o.width, o.height,
                 static_cast<int>(format), hr);
        return E_OUTOFMEMORY;
    }
    return S_OK;
}

namespace {

// --- Present -------------------------------------------------------------------------------------------------

D3DKMT_HANDLE allocation_of(D3D12DDI_HRESOURCE h)
{
    if (!h.pDrvPrivate)
        return 0;
    const ResourceObject* r = object<ResourceObject>(h);
    return r->heap ? r->heap->allocation : 0;
}

void APIENTRY present(D3D12DDI_HCOMMANDLIST, D3D12DDI_HCOMMANDQUEUE hq, const D3D12DDIARG_PRESENT_0001* args,
                      D3D12DDI_PRESENT_0051* out, D3D12DDI_PRESENT_CONTEXTS_0051* contexts,
                      D3D12DDI_PRESENT_HWQUEUES_0051* hw_queues)
{
    if (out)
        *out = {};
    if (contexts)
        *contexts = {};
    if (hw_queues)
        *hw_queues = {};
    if (!args || !out || !contexts || !hq.pDrvPrivate || !args->SurfacesToPresent || !args->phSurfacesToPresent) {
        VGD_WARN("Present: bad arguments");
        return;
    }
    const D3DKMT_HANDLE src = allocation_of(args->phSurfacesToPresent[0].hSurface);
    const D3DKMT_HANDLE dst = allocation_of(args->hDstResource);
    static std::atomic<unsigned> count{0};
    const unsigned n = count.fetch_add(1, std::memory_order_relaxed);
    if (!src) {
        VGD_WARN("Present: surface %p is not a swapchain buffer", args->phSurfacesToPresent[0].hSurface.pDrvPrivate);
        return;
    }
    out->BroadcastSrcAllocation[0] = src;
    out->BroadcastDstAllocation[0] = dst;
    out->AddedGpuWork = FALSE;
    out->BackBufferMultiplicity = 1;
    contexts->hContext = object<CommandQueueObject>(hq)->bridge->context();
    if (n < 4 || verbose())
        VGD_TRACE("Present #%u: surfaces %u, src 0x%x, dst 0x%x, flags 0x%x, interval %d, source %u, dirty %u, "
                  "context %p",
                  n, args->SurfacesToPresent, src, dst, args->Flags.Value, static_cast<int>(args->FlipInterval),
                  args->VidPnSourceID, args->DirtyRects, contexts->hContext);
}

UINT APIENTRY get_present_private_driver_data_size(D3D12DDI_HDEVICE, const D3D12DDIARG_PRESENT_0001*) { return 0; }

// --- DXGI ----------------------------------------------------------------------------------------------------
// D3D12 presents through the command list table; DXGI still asks these. The rest stay logging stubs.

HRESULT __stdcall get_gamma_caps(DXGI_DDI_ARG_GET_GAMMA_CONTROL_CAPS* a)
{
    if (a && a->pGammaCapabilities)
        *a->pGammaCapabilities = {};
    return S_OK;
}

HRESULT __stdcall set_display_mode(DXGI_DDI_ARG_SETDISPLAYMODE*) { return S_OK; }
HRESULT __stdcall set_resource_priority(DXGI_DDI_ARG_SETRESOURCEPRIORITY*) { return S_OK; }

HRESULT __stdcall query_resource_residency(DXGI_DDI_ARG_QUERYRESOURCERESIDENCY* a)
{
    for (SIZE_T i = 0; a && a->pStatus && i < a->Resources; i++)
        a->pStatus[i] = DXGI_DDI_RESIDENCY_FULLY_RESIDENT;
    return S_OK;
}

HRESULT __stdcall resolve_shared_resource(DXGI_DDI_ARG_RESOLVESHAREDRESOURCE*) { return S_OK; }
HRESULT __stdcall offer_resources(DXGI_DDI_ARG_OFFERRESOURCES*) { return S_OK; }

HRESULT __stdcall reclaim_resources(DXGI_DDI_ARG_RECLAIMRESOURCES* a)
{
    for (UINT i = 0; a && a->pDiscarded && i < a->Resources; i++)
        a->pDiscarded[i] = FALSE;
    return S_OK;
}

HRESULT __stdcall check_present_duration_support(DXGI_DDI_ARG_CHECKPRESENTDURATIONSUPPORT* a)
{
    if (a)
        a->ClosestSmallerDuration = a->ClosestLargerDuration = 0;
    return S_OK;
}

HRESULT __stdcall trim_residency_set(DXGI_DDI_ARG_TRIMRESIDENCYSET*) { return S_OK; }

} // namespace

void fill_present_ddi(D3D12DDI_DEVICE_FUNCS_CORE_0088& t)
{
    t.pfnGetPresentPrivateDriverDataSize = get_present_private_driver_data_size;
}

void fill_present_list_ddi(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& t) { t.pfnPresent = present; }

void fill_dxgi_ddi(DXGI1_4_DDI_BASE_FUNCTIONS& t)
{
    t.pfnGetGammaCaps = get_gamma_caps;
    t.pfnSetDisplayMode = set_display_mode;
    t.pfnSetResourcePriority = set_resource_priority;
    t.pfnQueryResourceResidency = query_resource_residency;
    t.pfnResolveSharedResource = resolve_shared_resource;
    t.pfnOfferResources = offer_resources;
    t.pfnReclaimResources = reclaim_resources;
    t.pfnCheckPresentDurationSupport = check_present_duration_support;
    t.pfnTrimResidencySet = trim_residency_set;
}

} // namespace vgd::umd
