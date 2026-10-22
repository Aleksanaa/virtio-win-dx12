// SPDX-License-Identifier: MIT
// OpenAdapter10_2 and the adapter function table. The runtime asks for caps before any device exists, and DXVK
// answers feature questions only for a device: the adapter creates one at the highest feature level for that and
// gives it to the first device created at that level.
#include "device.h"
#include "dxvk_bridge.h"
#include "viogpu_abi.h"
#include "vulkan_icd.h"

#include <cstring>
#include <iterator>

namespace vgd::umd {

namespace {

// The D3D11.1 DDI only. D3D10/10.1 applications on this adapter fail to create a device.
constexpr UINT64 kSupportedVersions[] = {D3D11_1_DDI_SUPPORTED};

struct Adapter {
    D3D10DDI_HRTADAPTER rt_adapter{};
    D3DDDI_ADAPTERCALLBACKS callbacks{};
    viogpu::AdapterInfo info{};
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    Com<ID3D11Device5> probe;   // at the highest level, for caps; null once a device has taken it
    dxvk::DeviceHandles probe_handles;
    D3D_FEATURE_LEVEL max_level = D3D_FEATURE_LEVEL(0);
    D3D11_FEATURE_DATA_DOUBLES doubles{};
    D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS compute_4x{};
    D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
    D3D11_FEATURE_DATA_SHADER_MIN_PRECISION_SUPPORT min_precision{};

    static Adapter* from(D3D10DDI_HADAPTER h) { return static_cast<Adapter*>(h.pDrvPrivate); }

    // The probe device and the answers taken from it.
    HRESULT bring_up()
    {
        if (max_level)
            return S_OK;
        HRESULT hr = dxvk::create_device(gipa, D3D_FEATURE_LEVEL(0), kSharingExtensions, std::size(kSharingExtensions),
                                         probe.put(), &probe_handles);
        if (FAILED(hr)) {
            VGD_WARN("OpenAdapter10_2: no DXVK device on the Venus ICD: 0x%08lx", hr);
            return hr;
        }
        max_level = probe->GetFeatureLevel();
        probe->CheckFeatureSupport(D3D11_FEATURE_DOUBLES, &doubles, sizeof(doubles));
        probe->CheckFeatureSupport(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS, &compute_4x, sizeof(compute_4x));
        probe->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options));
        probe->CheckFeatureSupport(D3D11_FEATURE_SHADER_MIN_PRECISION_SUPPORT, &min_precision, sizeof(min_precision));
        VGD_TRACE("adapter: DXVK on %s, feature level 0x%x", probe_handles.name, max_level);
        return S_OK;
    }
};

// The feature levels a 3D pipeline level stands for, and back.
D3D_FEATURE_LEVEL feature_level(D3D11DDI_3DPIPELINELEVEL level)
{
    switch (level) {
    case D3D11DDI_3DPIPELINELEVEL_10_0: return D3D_FEATURE_LEVEL_10_0;
    case D3D11DDI_3DPIPELINELEVEL_10_1: return D3D_FEATURE_LEVEL_10_1;
    case D3D11DDI_3DPIPELINELEVEL_11_0: return D3D_FEATURE_LEVEL_11_0;
    case D3D11_1DDI_3DPIPELINELEVEL_11_1: return D3D_FEATURE_LEVEL_11_1;
    case D3D11_1DDI_3DPIPELINELEVEL_9_1: return D3D_FEATURE_LEVEL_9_1;
    case D3D11_1DDI_3DPIPELINELEVEL_9_2: return D3D_FEATURE_LEVEL_9_2;
    case D3D11_1DDI_3DPIPELINELEVEL_9_3: return D3D_FEATURE_LEVEL_9_3;
    default: return D3D_FEATURE_LEVEL(0);
    }
}

SIZE_T APIENTRY calc_private_device_size(D3D10DDI_HADAPTER, const D3D10DDIARG_CALCPRIVATEDEVICESIZE* args)
{
    VGD_TRACE("CalcPrivateDeviceSize interface=0x%x version=0x%x flags=0x%x", args->Interface, args->Version,
              args->Flags);
    return sizeof(Device);
}

HRESULT APIENTRY create_device_ddi(D3D10DDI_HADAPTER h, D3D10DDIARG_CREATEDEVICE* args)
{
    Adapter* a = Adapter::from(h);
    HRESULT hr = a->bring_up();
    if (FAILED(hr))
        return hr;
    const D3D_FEATURE_LEVEL level = feature_level(D3D11DDI_EXTRACT_3DPIPELINELEVEL_FROM_FLAGS(args->Flags));
    VGD_TRACE("CreateDevice interface=0x%x version=0x%x flags=0x%x: feature level 0x%x", args->Interface,
              args->Version, args->Flags, level);
    if (args->Interface != D3D11_1_DDI_INTERFACE_VERSION || !level || level > a->max_level) {
        VGD_WARN("CreateDevice: interface 0x%x at feature level 0x%x not offered", args->Interface, level);
        return E_INVALIDARG;
    }
    Com<ID3D11Device5> d3d;
    dxvk::DeviceHandles handles;
    if (a->probe && a->probe->GetFeatureLevel() == level) {
        d3d = std::move(a->probe);
        handles = a->probe_handles;
    } else {
        hr = dxvk::create_device(a->gipa, level, kSharingExtensions, std::size(kSharingExtensions), d3d.put(),
                                 &handles);
        if (FAILED(hr))
            return hr;
    }
    return create_device(*args, a->info, a->gipa, std::move(d3d), handles);
}

HRESULT APIENTRY close_adapter(D3D10DDI_HADAPTER h)
{
    VGD_TRACE("CloseAdapter");
    delete Adapter::from(h);
    return S_OK;
}

HRESULT APIENTRY get_supported_versions(D3D10DDI_HADAPTER, UINT32* entries, UINT64* versions)
{
    constexpr UINT32 count = ARRAYSIZE(kSupportedVersions);
    if (versions) {
        if (*entries < count)
            return E_OUTOFMEMORY;
        for (UINT32 i = 0; i < count; i++)
            versions[i] = kSupportedVersions[i];
    }
    *entries = count;
    return S_OK;
}

HRESULT APIENTRY get_caps(D3D10DDI_HADAPTER h, const D3D10_2DDIARG_GETCAPS* args)
{
    if (!args || !args->pData)
        return E_INVALIDARG;
    Adapter* a = Adapter::from(h);
    std::memset(args->pData, 0, args->DataSize);
    HRESULT hr = a->bring_up();
    if (FAILED(hr))
        return hr;
    auto fits = [&](size_t size) { return args->DataSize >= size; };
    switch (args->Type) {
    case D3D11DDICAPS_THREADING:
        // Neither free-threaded nor command lists: the runtime serializes calls and records deferred contexts.
        break;
    case D3D11DDICAPS_SHADER:
        if (fits(sizeof(D3D11DDI_SHADER_CAPS))) {
            UINT caps = 0;
            if (a->doubles.DoublePrecisionFloatShaderOps)
                caps |= D3D11DDICAPS_SHADER_DOUBLES;
            if (a->compute_4x.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x)
                caps |= D3D11DDICAPS_SHADER_COMPUTE_PLUS_RAW_AND_STRUCTURED_BUFFERS_IN_SHADER_4_X;
            static_cast<D3D11DDI_SHADER_CAPS*>(args->pData)->Caps = caps;
        }
        break;
    case D3D11DDICAPS_3DPIPELINESUPPORT:
        if (fits(sizeof(D3D11DDI_3DPIPELINESUPPORT_CAPS))) {
            UINT caps = 0;
            for (auto level : {D3D11_1DDI_3DPIPELINELEVEL_9_1, D3D11_1DDI_3DPIPELINELEVEL_9_2,
                               D3D11_1DDI_3DPIPELINELEVEL_9_3, D3D11DDI_3DPIPELINELEVEL_10_0,
                               D3D11DDI_3DPIPELINELEVEL_10_1, D3D11DDI_3DPIPELINELEVEL_11_0,
                               D3D11_1DDI_3DPIPELINELEVEL_11_1})
                if (feature_level(level) <= a->max_level)
                    caps |= D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(level);
            static_cast<D3D11DDI_3DPIPELINESUPPORT_CAPS*>(args->pData)->Caps = caps;
        }
        break;
    case D3D11_1DDICAPS_D3D11_OPTIONS:
        if (fits(sizeof(D3D11_1DDI_D3D11_OPTIONS_DATA)))
            static_cast<D3D11_1DDI_D3D11_OPTIONS_DATA*>(args->pData)->OutputMergerLogicOp =
                a->options.OutputMergerLogicOp;
        break;
    case D3D11_1DDICAPS_ARCHITECTURE_INFO:
        break;   // not a tile-based deferred renderer
    case D3D11_1DDICAPS_SHADER_MIN_PRECISION_SUPPORT:
        if (fits(sizeof(D3D11_DDI_SHADER_MIN_PRECISION_SUPPORT_DATA))) {
            auto* p = static_cast<D3D11_DDI_SHADER_MIN_PRECISION_SUPPORT_DATA*>(args->pData);
            // The API's D3D11_SHADER_MIN_PRECISION_SUPPORT bits have the DDI's values.
            p->PixelShaderMinPrecision = a->min_precision.PixelShaderMinPrecision;
            p->AllOtherStagesMinPrecision = a->min_precision.AllOtherShaderStagesMinPrecision;
        }
        break;
    default:
        VGD_TRACE("GetCaps type %d (%u bytes): not ours, answered with zeros", static_cast<int>(args->Type),
                  args->DataSize);
        break;
    }
    return S_OK;
}

} // namespace

} // namespace vgd::umd

extern "C" __declspec(dllexport) HRESULT APIENTRY OpenAdapter10_2(D3D10DDIARG_OPENADAPTER* args)
{
    using namespace vgd::umd;
    log_init("vgd3d11");
    if (!args || !args->pAdapterCallbacks || !args->pAdapterFuncs_2)
        return E_INVALIDARG;
    VGD_TRACE("OpenAdapter10_2 interface=0x%x version=0x%x", args->Interface, args->Version);

    auto* adapter = new (std::nothrow) Adapter;
    if (!adapter)
        return E_OUTOFMEMORY;
    adapter->rt_adapter = args->hRTAdapter;
    adapter->callbacks = *args->pAdapterCallbacks;

    // The KMD's private adapter info tells us whether this is yttrium's viogpu3d and whether Venus is there.
    D3DDDICB_QUERYADAPTERINFO query{};
    query.pPrivateDriverData = &adapter->info;
    query.PrivateDriverDataSize = sizeof(adapter->info);
    HRESULT hr = adapter->callbacks.pfnQueryAdapterInfoCb(args->hRTAdapter.handle, &query);
    if (FAILED(hr) || adapter->info.IamVioGPU != vgd::viogpu::kIam ||
        !(adapter->info.SupportedCapsetIDs & (1ull << vgd::viogpu::kCapsetVenus))) {
        VGD_WARN("OpenAdapter10_2: not a viogpu adapter with Venus (hr=0x%08lx iam=0x%llx capsets=0x%llx)", hr,
                 adapter->info.IamVioGPU, adapter->info.SupportedCapsetIDs);
        delete adapter;
        return DXGI_ERROR_UNSUPPORTED;
    }
    adapter->gipa = load_venus_icd();
    if (!adapter->gipa) {
        delete adapter;
        return DXGI_ERROR_UNSUPPORTED;
    }

    *args->pAdapterFuncs_2 = {calc_private_device_size, create_device_ddi, close_adapter, get_supported_versions,
                              get_caps};
    args->hAdapter.pDrvPrivate = adapter;
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved)
{
    // Unloaded by FreeLibrary (reserved is null); at process exit nothing runs after us.
    if (reason == DLL_PROCESS_DETACH && !reserved)
        vgd::umd::log_shutdown();
    return TRUE;
}
