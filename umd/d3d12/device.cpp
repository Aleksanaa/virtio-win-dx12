// SPDX-License-Identifier: MIT
#include "device.h"
#include "ddi_slots.h"
#include "device_core.h"
#include "log.h"
#include "objects.h"
#include "stubs.h"


#include <cstring>
#include <new>

namespace vgd::umd {

namespace {

// The DDI build we negotiate (GetSupportedVersions) and the table versions that belong to it.
constexpr UINT kBuildVersion = D3D12DDI_BUILD_VERSION_0092;

template <class Table>
HRESULT check_table_size(D3D12DDI_TABLE_TYPE type, SIZE_T size)
{
    if (size == sizeof(Table))
        return S_OK;
    VGD_WARN("FillDDITable type=%d: size %llu, expected %llu", static_cast<int>(type),
             static_cast<unsigned long long>(size), static_cast<unsigned long long>(sizeof(Table)));
    return E_INVALIDARG;
}

// --- Extended features: we offer none (no render-pass DDI, shader-cache callbacks, video, ...). -----------------

HRESULT APIENTRY get_supported_extended_features(D3D12DDI_HDEVICE, UINT32* count, D3D12DDI_FEATURE_0020*)
{
    if (!count)
        return E_INVALIDARG;
    *count = 0;
    return S_OK;
}

HRESULT APIENTRY get_supported_extended_feature_versions(D3D12DDI_HDEVICE, D3D12DDI_FEATURE_0020 feature,
                                                         UINT32* count, UINT32*)
{
    VGD_WARN("GetSupportedExtendedFeatureVersions(%d): feature not offered", static_cast<int>(feature));
    if (count)
        *count = 0;
    return E_INVALIDARG;
}

HRESULT APIENTRY enable_extended_feature(D3D12DDI_HDEVICE, D3D12DDI_FEATURE_0020 feature, UINT32 version)
{
    VGD_WARN("EnableExtendedFeature(%d, %u): feature not offered", static_cast<int>(feature), version);
    return E_INVALIDARG;
}

HRESULT APIENTRY set_extended_feature_callbacks(D3D12DDI_HDEVICE, D3D12DDI_TABLE_TYPE table, const void*, SIZE_T size)
{
    VGD_TRACE("SetExtendedFeatureCallbacks table=%d size=%llu (ignored)", static_cast<int>(table),
              static_cast<unsigned long long>(size));
    return S_OK;
}

} // namespace

SIZE_T device_private_size() { return sizeof(Device); }

HRESULT create_device(const D3D12DDIARG_CREATEDEVICE_0003& args, const CommandListTables& tables,
                      const viogpu::AdapterInfo& info, std::shared_ptr<Engine> engine)
{
    if ((args.Version >> 16) != kBuildVersion) {
        VGD_WARN("CreateDevice: runtime picked build %u, we only fill tables for %u", args.Version >> 16, kBuildVersion);
        return E_INVALIDARG;
    }
    auto* d = new (args.hDrvDevice.pDrvPrivate) Device;
    d->rt_device = args.hRTDevice;
    d->command_list_tables = &tables;
    d->kt_callbacks = args.pKTCallbacks;
    d->um_callbacks = args.p12UMCallbacks_0062;
    d->allocation_alias = info.Flags.supports_allocation_alias;
    d->engine = std::move(engine);
    d->d3d = d->engine->device.get();
    d->d3d2 = d->engine->device.as<ID3D12Device2>();
    d->d3d4 = d->engine->device.as<ID3D12Device4>();
    d->d3d8 = d->engine->device.as<ID3D12Device8>();
    // GetTimestampFrequency comes from the KMD, which reports the performance counter's; resolved timestamps are
    // the Vulkan device's ticks (not rescaled yet).
    VGD_TRACE("CreateDevice: ok on %s (flags 0x%x)", d->engine->name.c_str(), static_cast<unsigned>(args.Flags));
    return S_OK;
}

void destroy_device(D3D12DDI_HDEVICE h)
{
    if (Device* d = Device::from(h))
        d->~Device();
}

HRESULT fill_ddi_table(D3D12DDI_TABLE_TYPE type, void* table, SIZE_T size, UINT index, D3D12DDI_HRTTABLE rt_table,
                       CommandListTables& tables)
{
    HRESULT hr = E_NOTIMPL;
    switch (type) {
    case D3D12DDI_TABLE_TYPE_DEVICE_CORE: {
        using T = D3D12DDI_DEVICE_FUNCS_CORE_0088;
        if (FAILED(hr = check_table_size<T>(type, size)))
            break;
        auto& t = *static_cast<T*>(table);
        VGD_FILL_STUBS(t, "device", VGD_SLOTS_DEVICE_CORE_0088);
        fill_device_core(t);
        fill_present_ddi(t);
        break;
    }
    case D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D: {
        using T = D3D12DDI_COMMAND_LIST_FUNCS_3D_0092;
        if (FAILED(hr = check_table_size<T>(type, size)))
            break;
        auto& t = *static_cast<T*>(table);
        VGD_FILL_STUBS(t, "cmdlist", VGD_SLOTS_COMMAND_LIST_3D_0092);
        fill_command_list_ddi(t);
        fill_query_list_ddi(t);
        fill_indirect_list_ddi(t);
        fill_present_list_ddi(t);
        if (index < CommandListTables::kMax) {
            tables.rt[index] = rt_table;
            if (index >= tables.count)
                tables.count = index + 1;
        } else {
            VGD_WARN("FillDDITable: command list table %u beyond the %u we keep", index, CommandListTables::kMax);
        }
        break;
    }
    case D3D12DDI_TABLE_TYPE_COMMAND_QUEUE_3D: {
        using T = D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001;
        if (FAILED(hr = check_table_size<T>(type, size)))
            break;
        auto& t = *static_cast<T*>(table);
        VGD_FILL_STUBS(t, "queue", VGD_SLOTS_COMMAND_QUEUE_CORE_0001);
        fill_command_queue_ddi(t);
        fill_resource_queue_ddi(t);
        break;
    }
    case D3D12DDI_TABLE_TYPE_DXGI: {
        using T = DXGI1_4_DDI_BASE_FUNCTIONS;
        if (FAILED(hr = check_table_size<T>(type, size)))
            break;
        auto& t = *static_cast<T*>(table);
        VGD_FILL_STUBS(t, "dxgi", VGD_SLOTS_DXGI_1_4);
        fill_dxgi_ddi(t);
        break;
    }
    case D3D12DDI_TABLE_TYPE_0020_EXTENDED_FEATURES: {
        using T = D3D12DDI_EXTENDED_FEATURES_FUNCS_0021;
        if (FAILED(hr = check_table_size<T>(type, size)))
            break;
        auto& t = *static_cast<T*>(table);
        VGD_FILL_STUBS(t, "extended", VGD_SLOTS_EXTENDED_FEATURES_0021);
        t.pfnGetSupportedExtendedFeatures = get_supported_extended_features;
        t.pfnGetSupportedExtendedFeatureVersions = get_supported_extended_feature_versions;
        t.pfnEnableExtendedFeature = enable_extended_feature;
        t.pfnSetExtendedFeatureCallbacks = set_extended_feature_callbacks;
        break;
    }
    default:
        break;
    }
    VGD_TRACE("FillDDITable type=%d size=%llu index=%u rt_table=%p -> 0x%08lx", static_cast<int>(type),
              static_cast<unsigned long long>(size), index, rt_table.handle, hr);
    return hr;
}

} // namespace vgd::umd
