// SPDX-License-Identifier: MIT
// OpenAdapter12 and the adapter function table. Device creation is the next milestone; for now every
// runtime query is logged so the first guest run shows exactly what the runtime asks of us.
#include "wdk.h"
#include "log.h"
#include "viogpu_abi.h"

#include <new>

namespace vgd::umd {

namespace {

// Proven against the Windows 11 runtime by other UMDs; newer versions only add tables we would stub anyway.
constexpr UINT64 kSupportedVersions[] = {D3D12DDI_SUPPORTED_0092};

struct Adapter {
    D3D12DDI_HRTADAPTER rt_adapter{};
    const D3DDDI_ADAPTERCALLBACKS* callbacks = nullptr;
    viogpu::AdapterInfo info{};

    static Adapter* from(D3D12DDI_HADAPTER h) { return static_cast<Adapter*>(h.pDrvPrivate); }
};

SIZE_T APIENTRY calc_private_device_size(D3D12DDI_HADAPTER, const D3D12DDIARG_CALCPRIVATEDEVICESIZE* args)
{
    VGD_TRACE("CalcPrivateDeviceSize interface=0x%x version=0x%x flags=0x%x", args->Interface, args->Version,
              args->Flags);
    return 64;
}

HRESULT APIENTRY create_device(D3D12DDI_HADAPTER, const D3D12DDIARG_CREATEDEVICE_0003* args)
{
    VGD_WARN("CreateDevice interface=0x%x version=0x%x: not implemented yet", args->Interface, args->Version);
    return E_NOTIMPL;
}

HRESULT APIENTRY close_adapter(D3D12DDI_HADAPTER h)
{
    VGD_TRACE("CloseAdapter");
    delete Adapter::from(h);
    return S_OK;
}

HRESULT APIENTRY get_supported_versions(D3D12DDI_HADAPTER, UINT32* entries, UINT64* versions)
{
    constexpr UINT32 count = ARRAYSIZE(kSupportedVersions);
    if (versions) {
        if (*entries < count)
            return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        for (UINT32 i = 0; i < count; i++)
            versions[i] = kSupportedVersions[i];
    }
    *entries = count;
    return S_OK;
}

HRESULT APIENTRY get_caps(D3D12DDI_HADAPTER, const D3D12DDIARG_GETCAPS* args)
{
    VGD_TRACE("GetCaps type=%d size=%u info=%p", static_cast<int>(args->Type), args->DataSize, args->pInfo);
    return E_NOTIMPL;
}

HRESULT APIENTRY get_optional_ddi_tables(D3D12DDI_HADAPTER, UINT32* entries, D3D12DDI_TABLE_REQUEST*)
{
    VGD_TRACE("GetOptionalDDITables");
    *entries = 0;
    return S_OK;
}

HRESULT APIENTRY fill_ddi_table(D3D12DDI_HADAPTER, D3D12DDI_TABLE_TYPE type, void*, SIZE_T size, UINT version,
                                D3D12DDI_HRTTABLE)
{
    VGD_TRACE("FillDDITable type=%d size=%llu version=0x%x", static_cast<int>(type), static_cast<unsigned long long>(size),
              version);
    return E_NOTIMPL;
}

void APIENTRY destroy_device(D3D12DDI_HDEVICE)
{
    VGD_TRACE("DestroyDevice");
}

} // namespace

} // namespace vgd::umd

extern "C" __declspec(dllexport) HRESULT APIENTRY OpenAdapter12(D3D12DDIARG_OPENADAPTER* args)
{
    using namespace vgd::umd;
    log_init();
    if (!args || !args->pAdapterCallbacks || !args->pAdapterFuncs)
        return E_INVALIDARG;

    auto* adapter = new (std::nothrow) Adapter;
    if (!adapter)
        return E_OUTOFMEMORY;
    adapter->rt_adapter = args->hRTAdapter;
    adapter->callbacks = args->pAdapterCallbacks;

    // The KMD's private adapter info tells us whether this is yttrium's viogpu3d and whether Venus is there.
    D3DDDICB_QUERYADAPTERINFO query{};
    query.pPrivateDriverData = &adapter->info;
    query.PrivateDriverDataSize = sizeof(adapter->info);
    HRESULT hr = adapter->callbacks->pfnQueryAdapterInfoCb(args->hRTAdapter.handle, &query);
    if (FAILED(hr) || adapter->info.IamVioGPU != vgd::viogpu::kIam) {
        VGD_WARN("OpenAdapter12: not a viogpu adapter (hr=0x%08lx iam=0x%llx)", hr, adapter->info.IamVioGPU);
        delete adapter;
        return DXGI_ERROR_UNSUPPORTED;
    }
    const bool venus = adapter->info.SupportedCapsetIDs & (1ull << vgd::viogpu::kCapsetVenus);
    VGD_TRACE("OpenAdapter12: viogpu flags 3d=%u blob=%u hostvisible=%u ctxinit=%u capsets=0x%llx venus=%d",
              adapter->info.Flags.Supports3d, adapter->info.Flags.has_resource_blob,
              adapter->info.Flags.has_host_visible, adapter->info.Flags.has_context_init,
              adapter->info.SupportedCapsetIDs, venus);
    if (!venus) {
        delete adapter;
        return DXGI_ERROR_UNSUPPORTED;
    }

    *args->pAdapterFuncs = {
        calc_private_device_size, create_device, close_adapter, get_supported_versions,
        get_caps, get_optional_ddi_tables, fill_ddi_table, destroy_device,
    };
    args->hAdapter.pDrvPrivate = adapter;
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
