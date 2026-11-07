// SPDX-License-Identifier: MIT
// Device creation and destruction, the device-wide DDIs (flush, counters, deferred contexts) and the DDI tables.
#include "device.h"
#include "ddi_slots.h"
#include "stubs.h"

namespace vgd::umd {

namespace {

void APIENTRY destroy_device(D3D10DDI_HDEVICE h)
{
    VGD_TRACE("DestroyDevice");
    Device* d = Device::from(h);
    if (d->ctx) {
        d->ctx->ClearState();
        d->wait_idle();
    }
    destroy_render_fence(*d);
    if (d->present_context) {
        D3DDDICB_DESTROYCONTEXT dc{d->present_context};
        if (FAILED(d->kt.pfnDestroyContextCb(d->rt_device.handle, &dc)))
            VGD_WARN("DestroyDevice: DestroyContextCb failed");
    }
    if (d->paging_queue) {
        D3DDDI_DESTROYPAGINGQUEUE dq{d->paging_queue};
        if (FAILED(d->kt.pfnDestroyPagingQueueCb(d->rt_device.handle, &dq)))
            VGD_WARN("DestroyDevice: DestroyPagingQueueCb failed");
    }
    if (d->idle_event)
        CloseHandle(d->idle_event);
    d->~Device();
}

// The runtime moved the table; we keep no pointers into it.
void APIENTRY relocate_device_funcs(D3D10DDI_HDEVICE, D3D11_1DDI_DEVICEFUNCS*) {}

BOOL APIENTRY flush(D3D10DDI_HDEVICE h, UINT flags)
{
    // Another device or process may read a shared surface once this returns, ordered by nothing else: our rendering
    // runs on DXVK's Vulkan queue, which the runtime does not schedule. So complete it.
    Device* d = Device::from(h);
    d->flush_clears();
    if (FAILED(d->wait_idle()))
        d->set_error(D3DDDIERR_DEVICEREMOVED);
    return TRUE;
}

// No performance counters.
void APIENTRY check_counter_info(D3D10DDI_HDEVICE, D3D10DDI_COUNTER_INFO* info)
{
    *info = {};
}

void APIENTRY check_counter(D3D10DDI_HDEVICE h, D3D10DDI_QUERY, D3D10DDI_COUNTER_TYPE*, UINT*, LPSTR, UINT*, LPSTR,
                            UINT*, LPSTR, UINT*)
{
    Device::from(h)->set_error(DXGI_DDI_ERR_UNSUPPORTED);
}

// The D3D10 text filter (R1_UNORM) is not supported; the size only matters for it.
void APIENTRY set_text_filter_size(D3D10DDI_HDEVICE, UINT, UINT) {}

// Deferred contexts are the runtime's: we report no command-list support (D3D11DDICAPS_THREADING).
void APIENTRY check_deferred_context_handle_sizes(D3D10DDI_HDEVICE, UINT* count, D3D11DDI_HANDLESIZE*)
{
    *count = 0;
}

} // namespace

void Device::flush_clears()
{
    // An interop transition ends DXVK's pass without suspending it, which runs all pending clears, not only this
    // surface's. A flush suspends the pass and keeps them.
    if (pending_clear && FAILED(dxvk::flush_shared_clears(d3d.get(), pending_clear.get())))
        VGD_WARN("flush_clears: the interop transition failed");
    pending_clear = nullptr;
}

HRESULT Device::wait_idle()
{
    // DXVK sets the event when the flushed submission completes, by its own submission tracking. Not an ID3D11Fence:
    // DXVK's ID3D11Fence signals never completed here (their timeline semaphores stayed at 0 on the host;
    // d3d11probe's fence test reproduces it).
    if (!idle_event)
        idle_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!idle_event)
        return E_OUTOFMEMORY;
    ctx->Flush1(D3D11_CONTEXT_TYPE_ALL, idle_event);
    // In slices, so a GPU that never finishes shows in the log rather than as a silent hang. At 6 s, once per
    // process, also where every thread is: DXVK's CS and submission threads show what holds the work back.
    static std::atomic<bool> stacks_logged{false};
    for (unsigned waited = 2; WaitForSingleObject(idle_event, 2000) == WAIT_TIMEOUT; waited += 2) {
        VGD_WARN("wait_idle: rendering not finished after %us", waited);
        if (waited == 6 && !stacks_logged.exchange(true))
            log_thread_stacks("wait_idle stalled");
    }
    return S_OK;
}

HRESULT create_device(D3D10DDIARG_CREATEDEVICE& args, const viogpu::AdapterInfo& info, PFN_vkGetInstanceProcAddr gipa,
                      Com<ID3D11Device5> d3d, const dxvk::DeviceHandles& handles)
{
    auto* d = new (args.hDrvDevice.pDrvPrivate) Device;
    d->rt_device = args.hRTDevice;
    d->rt_core_layer = args.hRTCoreLayer;
    d->kt = *args.pKTCallbacks;
    d->um = *args.p11UMCallbacks;
    d->dxgi = args.DXGIBaseDDI.pDXGIBaseCallbacks;
    d->allocation_alias = info.Flags.supports_allocation_alias;
    d->d3d = std::move(d3d);
    Com<ID3D11DeviceContext> ctx;
    d->d3d->GetImmediateContext(ctx.put());
    d->ctx = ctx.as<ID3D11DeviceContext4>();
    HRESULT hr = d->ctx ? d->vulkan.init(gipa, handles.instance, handles.physical, handles.device) : E_NOINTERFACE;
    if (FAILED(hr)) {
        VGD_WARN("CreateDevice: DXVK's device is not usable: 0x%08lx", hr);
        d->~Device();
        return hr;
    }
    // A context of the runtime's, for presents and for the keyed mutex signals of shared surfaces (ReleaseSync fails
    // without one, even on a device that never presents). Physical, not virtual: yttrium's KMD rejects virtual
    // application contexts. Nothing is submitted to it; the runtime queues on it.
    D3DDDICB_CREATECONTEXT cc{};
    cc.NodeOrdinal = 0;
    cc.EngineAffinity = 1;
    hr = d->kt.pfnCreateContextCb(d->rt_device.handle, &cc);
    if (FAILED(hr) || !cc.hContext) {
        VGD_WARN("CreateDevice: CreateContextCb failed: 0x%08lx", hr);
        d->~Device();
        return FAILED(hr) ? hr : E_FAIL;
    }
    d->present_context = cc.hContext;

    auto& t = *args.p11_1DeviceFuncs;
    VGD_FILL_STUBS(t, "device", VGD_SLOTS_D3D11_1_DEVICE);
    t.pfnDestroyDevice = destroy_device;
    t.pfnRelocateDeviceFuncs = relocate_device_funcs;
    t.pfnFlush = flush;
    t.pfnCheckCounterInfo = check_counter_info;
    t.pfnCheckCounter = check_counter;
    t.pfnSetTextFilterSize = set_text_filter_size;
    t.pfnCheckDeferredContextHandleSizes = check_deferred_context_handle_sizes;
    t.pfnCheckFormatSupport = check_format_support;
    t.pfnCheckMultisampleQualityLevels = check_multisample_quality_levels;
    fill_resource_ddi(t);
    fill_view_ddi(t);
    fill_pipeline_ddi(t);
    fill_draw_ddi(t);
    fill_query_ddi(t);

    auto& dxgi = *args.DXGIBaseDDI.pDXGIDDIBaseFunctions3;
    VGD_FILL_STUBS(dxgi, "dxgi", VGD_SLOTS_DXGI_1_2);
    fill_dxgi_ddi(dxgi);

    VGD_TRACE("CreateDevice: ok on %s, feature level 0x%x (external memory %d, drm modifiers %d, alias %d)",
              handles.name, d->d3d->GetFeatureLevel(), d->vulkan.external_memory, d->vulkan.drm_modifier,
              d->allocation_alias);
    return S_OK;
}

} // namespace vgd::umd
