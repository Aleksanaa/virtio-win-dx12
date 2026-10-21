// SPDX-License-Identifier: MIT
// Swapchain buffers, Present and the DXGI table.
//
// DXGI creates swapchain buffers as committed resources on PRIMARY heaps, and the compositor (DWM, through
// yttrium's D3D10 UMD) opens their runtime allocation and imports its host resource into its own Venus context.
// That resource must be a dma-buf exported from Vulkan memory, and the stock KMD creates a blob only in the context
// of the device that first opens the allocation, here the D3D12 runtime's device, which has no Venus context. So:
//   1. core makes the buffer a linear texture on exported memory (our ICD build exports it as an NT handle);
//   2. we open that handle on a kernel device of our own, which gives an allocation handle of this process;
//   3. the runtime allocation is created as an alias of it (viogpu::AllocationAlias, our KMD patch): it names the
//      same host resource, which lives until both are gone.
// Present is then zero-copy: the buffer's allocation goes to the runtime, which presents it on the queue's
// context, where our queue bridge has ordered it after the rendering.
#include "device.h"
#include "log.h"
#include "objects.h"
#include "viogpu_abi.h"

#include <winternl.h>   // OBJECT_ATTRIBUTES, for D3DKMTShareObjects

#include <atomic>
#include <memory>
#include <new>
#include <vector>

namespace vgd::umd {

// --- Our kernel device ---------------------------------------------------------------------------------------

struct KmtDevice {
    PFND3DKMT_ENUMADAPTERS2 enum_adapters = nullptr;
    PFND3DKMT_QUERYADAPTERINFO query_adapter_info = nullptr;
    PFND3DKMT_CLOSEADAPTER close_adapter = nullptr;
    PFND3DKMT_CREATEDEVICE create_device = nullptr;
    PFND3DKMT_DESTROYDEVICE destroy_device = nullptr;
    PFND3DKMT_QUERYRESOURCEINFOFROMNTHANDLE query_resource_info = nullptr;
    PFND3DKMT_OPENRESOURCEFROMNTHANDLE open_resource = nullptr;
    PFND3DKMT_DESTROYALLOCATION destroy_allocation = nullptr;
    PFND3DKMT_SHAREOBJECTS share_objects = nullptr;
    D3DKMT_HANDLE adapter = 0;
    D3DKMT_HANDLE device = 0;

    ~KmtDevice()
    {
        if (device) {
            D3DKMT_DESTROYDEVICE dd{device};
            if (destroy_device(&dd) < 0)
                VGD_WARN("present: D3DKMTDestroyDevice(0x%x) failed", device);
        }
        if (adapter) {
            D3DKMT_CLOSEADAPTER ca{adapter};
            if (close_adapter(&ca) < 0)
                VGD_WARN("present: D3DKMTCloseAdapter(0x%x) failed", adapter);
        }
    }

    // Opens a shared NT handle: one allocation, which `resource` holds open until close().
    HRESULT open(HANDLE nt, D3DKMT_HANDLE* resource, D3DKMT_HANDLE* allocation)
    {
        D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE q{};
        q.hDevice = device;
        q.hNtHandle = nt;
        NTSTATUS s = query_resource_info(&q);
        if (s < 0 || q.NumAllocations != 1) {
            VGD_WARN("present: QueryResourceInfoFromNtHandle failed: 0x%08lx, %u allocations", s, q.NumAllocations);
            return E_FAIL;
        }
        std::vector<char> runtime(q.PrivateRuntimeDataSize), total(q.TotalPrivateDriverDataSize),
            res_private(q.ResourcePrivateDriverDataSize);
        D3DDDI_OPENALLOCATIONINFO2 info{};
        D3DKMT_OPENRESOURCEFROMNTHANDLE o{};
        o.hDevice = device;
        o.hNtHandle = nt;
        o.NumAllocations = 1;
        o.pOpenAllocationInfo2 = &info;
        o.PrivateRuntimeDataSize = q.PrivateRuntimeDataSize;
        o.pPrivateRuntimeData = runtime.data();
        o.ResourcePrivateDriverDataSize = q.ResourcePrivateDriverDataSize;
        o.pResourcePrivateDriverData = res_private.data();
        o.TotalPrivateDriverDataBufferSize = q.TotalPrivateDriverDataSize;
        o.pTotalPrivateDriverDataBuffer = total.data();
        s = open_resource(&o);
        if (s < 0 || !info.hAllocation) {
            VGD_WARN("present: OpenResourceFromNtHandle failed: 0x%08lx", s);
            return E_FAIL;
        }
        *resource = o.hResource;
        *allocation = info.hAllocation;
        return S_OK;
    }

    void close(D3DKMT_HANDLE resource)
    {
        D3DKMT_DESTROYALLOCATION d{};
        d.hDevice = device;
        d.hResource = resource;
        NTSTATUS s = destroy_allocation(&d);
        if (s < 0)
            VGD_WARN("present: closing opened resource 0x%x failed: 0x%08lx", resource, s);
    }
};

void KmtDeviceDeleter::operator()(KmtDevice* k) const { delete k; }

namespace {

// A kernel device of our own on the viogpu adapter (the runtime does not give us its own device's handle).
KmtDevice* kmt_device(Device& d)
{
    if (d.kmt)
        return d.kmt.get();
    HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    if (!gdi)
        return nullptr;
    std::unique_ptr<KmtDevice, KmtDeviceDeleter> k(new (std::nothrow) KmtDevice);
    if (!k)
        return nullptr;
    auto get = [&](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(reinterpret_cast<void*>(GetProcAddress(gdi, name)));
        return fn != nullptr;
    };
    if (!get(k->enum_adapters, "D3DKMTEnumAdapters2") || !get(k->query_adapter_info, "D3DKMTQueryAdapterInfo") ||
        !get(k->close_adapter, "D3DKMTCloseAdapter") || !get(k->create_device, "D3DKMTCreateDevice") ||
        !get(k->destroy_device, "D3DKMTDestroyDevice") ||
        !get(k->query_resource_info, "D3DKMTQueryResourceInfoFromNtHandle") ||
        !get(k->open_resource, "D3DKMTOpenResourceFromNtHandle") ||
        !get(k->destroy_allocation, "D3DKMTDestroyAllocation") ||
        !get(k->share_objects, "D3DKMTShareObjects")) {
        VGD_WARN("present: gdi32 lacks the D3DKMT entry points we need");
        return nullptr;
    }

    D3DKMT_ENUMADAPTERS2 e{};
    if (k->enum_adapters(&e) < 0 || !e.NumAdapters)
        return nullptr;
    std::vector<D3DKMT_ADAPTERINFO> adapters(e.NumAdapters);
    e.pAdapters = adapters.data();
    if (k->enum_adapters(&e) < 0)
        return nullptr;
    for (ULONG i = 0; i < e.NumAdapters; i++) {
        viogpu::AdapterInfo info{};
        D3DKMT_QUERYADAPTERINFO q{};
        q.hAdapter = adapters[i].hAdapter;
        q.Type = KMTQAITYPE_UMDRIVERPRIVATE;
        q.pPrivateDriverData = &info;
        q.PrivateDriverDataSize = sizeof(info);
        const bool ours = !k->adapter && k->query_adapter_info(&q) >= 0 && info.IamVioGPU == viogpu::kIam;
        if (ours) {
            k->adapter = adapters[i].hAdapter;
        } else {
            D3DKMT_CLOSEADAPTER ca{adapters[i].hAdapter};
            if (k->close_adapter(&ca) < 0)
                VGD_WARN("present: D3DKMTCloseAdapter(0x%x) failed", adapters[i].hAdapter);
        }
    }
    if (!k->adapter) {
        VGD_WARN("present: no viogpu adapter among %lu", e.NumAdapters);
        return nullptr;
    }
    D3DKMT_CREATEDEVICE cd{};
    cd.hAdapter = k->adapter;
    NTSTATUS s = k->create_device(&cd);
    if (s < 0) {
        VGD_WARN("present: D3DKMTCreateDevice failed: 0x%08lx", s);
        return nullptr;
    }
    k->device = cd.hDevice;
    VGD_TRACE("present: kernel device 0x%x on adapter 0x%x", k->device, k->adapter);
    d.kmt = std::move(k);
    return d.kmt.get();
}

// The format yttrium's D3D10 UMD knows the buffer by (virgl, or pipe where it has no virgl translation); 0 if none.
ULONG virgl_format(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return viogpu::kVirglFormatB8G8R8A8Unorm;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return viogpu::kVirglFormatB8G8R8A8Srgb;
    case DXGI_FORMAT_B8G8R8X8_UNORM: return viogpu::kVirglFormatB8G8R8X8Unorm;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return viogpu::kVirglFormatR8G8B8A8Unorm;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return viogpu::kVirglFormatR8G8B8A8Srgb;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return viogpu::kVirglFormatR10G10B10A2Unorm;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return viogpu::kPipeFormatR16G16B16A16Float;
    default: return 0;
    }
}

// Our ICD build exports memory as the NT handle value in place of an fd (icd/patches/0003): the CRT cannot wrap
// a shared graphics resource handle in a descriptor.
HANDLE handle_of_fd(int fd) { return fd < 0 ? INVALID_HANDLE_VALUE : reinterpret_cast<HANDLE>(static_cast<intptr_t>(fd)); }

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

    vgd::Device::ExportedTexture exported;
    HRESULT hr = d.core->create_presentable_texture(desc, args.pCastableFormats, args.NumCastableFormats,
                                                    &out.resource, &exported);
    if (FAILED(hr)) {
        VGD_WARN("present: presentable texture failed: 0x%08lx", hr);
        return E_OUTOFMEMORY;
    }
    const HANDLE nt = handle_of_fd(exported.fd);
    KmtDevice* kmt = kmt_device(d);
    D3DKMT_HANDLE opened_resource = 0, source = 0;
    if (kmt && nt != INVALID_HANDLE_VALUE)
        hr = kmt->open(nt, &opened_resource, &source);
    else
        hr = E_FAIL;
    if (nt != INVALID_HANDLE_VALUE)
        CloseHandle(nt);   // the opened resource keeps the allocation
    if (FAILED(hr)) {
        VGD_WARN("present: could not open the exported memory (fd %d, handle %p)", exported.fd, nt);
        out.resource.reset();
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
    alias.exchange.Size = exported.size;
    alias.exchange.BlobMem = viogpu::kBlobMemHost3d;
    alias.exchange.BlobFlags = viogpu::kBlobFlagShareable;
    alias.exchange.Stride = exported.row_pitch;
    alias.Magic = viogpu::kAllocationAliasMagic;
    alias.SourceAllocation = source;
    viogpu::ResourceExchange res_exchange{};

    D3D12DDI_ALLOCATION_INFO_0022 info{};
    info.pPrivateDriverData = &alias;
    info.PrivateDriverDataSize = sizeof(alias);
    info.Flags = D3D12DDI_ALLOCATION_INFO_FLAGS_0022_PRIMARY;
    D3D12DDICB_ALLOCATE_0022 cb{};
    cb.pPrivateDriverData = &res_exchange;
    cb.PrivateDriverDataSize = sizeof(res_exchange);
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
        out.resource.reset();
        return E_OUTOFMEMORY;
    }
    out.allocation = info.hAllocation;
    out.rt_resource = rt_resource;
    VGD_TRACE("present: swapchain buffer allocation 0x%x (km resource 0x%x) aliases 0x%x: %llu bytes, pitch %u, "
              "virgl format %lu",
              info.hAllocation, cb.hKMResource, source, static_cast<unsigned long long>(exported.size),
              exported.row_pitch, format);
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
// import (icd/patches/0003, 0005), under a linear image of the same size and format (core import_texture).

namespace {

// The DXGI format of a format yttrium's D3D10 UMD knows a buffer by (virgl_format's inverse), with its sRGB
// sibling for views (UNKNOWN if none); UNKNOWN if none.
DXGI_FORMAT dxgi_format(ULONG format, DXGI_FORMAT* srgb)
{
    *srgb = DXGI_FORMAT_UNKNOWN;
    switch (format) {
    case viogpu::kVirglFormatB8G8R8A8Unorm:
    case viogpu::kVirglFormatB8G8R8A8Srgb:
        *srgb = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case viogpu::kVirglFormatB8G8R8X8Unorm: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case viogpu::kVirglFormatR8G8B8A8Unorm:
    case viogpu::kVirglFormatR8G8B8A8Srgb:
        *srgb = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case viogpu::kVirglFormatR10G10B10A2Unorm: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case viogpu::kPipeFormatR16G16B16A16Float: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

} // namespace

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
    D3DKMT_HANDLE resource = args.hKMResource.handle;
    OBJECT_ATTRIBUTES attrs{};
    attrs.Length = sizeof(attrs);
    HANDLE nt = nullptr;
    NTSTATUS s = kmt->share_objects(1, &resource, &attrs, SHARED_ALLOCATION_ALL_ACCESS, &nt);
    if (s < 0 || !nt) {
        VGD_WARN("OpenHeapAndResource: D3DKMTShareObjects(0x%x) failed: 0x%08lx", resource, s);
        return E_OUTOFMEMORY;
    }
    if (reinterpret_cast<uintptr_t>(nt) > INT_MAX) {
        CloseHandle(nt);
        return E_OUTOFMEMORY;
    }
    bool consumed = false;
    HRESULT hr = d.core->import_texture(desc, &srgb, srgb != DXGI_FORMAT_UNKNOWN ? 1 : 0,
                                        static_cast<int>(reinterpret_cast<intptr_t>(nt)), e.Size, &out.resource,
                                        &consumed);
    if (!consumed)
        CloseHandle(nt);
    if (FAILED(hr)) {
        VGD_WARN("OpenHeapAndResource: import of %lux%lu format %d failed: 0x%08lx", o.width, o.height,
                 static_cast<int>(format), hr);
        return E_OUTOFMEMORY;
    }
    if (out.resource->size() != e.Size)
        VGD_TRACE("OpenHeapAndResource: our linear image needs %llu bytes of the %llu shared",
                  static_cast<unsigned long long>(out.resource->size()), static_cast<unsigned long long>(e.Size));
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
