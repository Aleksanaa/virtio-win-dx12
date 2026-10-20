// SPDX-License-Identifier: MIT
// D3D12DDI_DEVICE_FUNCS_CORE_0088 slots we implement. Everything else in the table is a logging stub (stubs.h).
#include "device_core.h"
#include "log.h"

namespace vgd::umd {

namespace {

// Descriptors are CPU-side records we define. 64 bytes holds any Vulkan descriptor we will store; once
// VK_EXT_descriptor_heap is in use this becomes the device's descriptor size for the heap type.
constexpr UINT kDescriptorSize = 64;

UINT APIENTRY get_descriptor_size_in_bytes(D3D12DDI_HDEVICE, D3D12DDI_DESCRIPTOR_HEAP_TYPE)
{
    return kDescriptorSize;
}

void APIENTRY check_multisample_quality_levels(D3D12DDI_HDEVICE, DXGI_FORMAT, UINT samples,
                                               D3D12DDI_MULTISAMPLE_QUALITY_LEVEL_FLAGS, UINT* levels)
{
    // Single-sampled only until MSAA images exist in core.
    *levels = samples == 1 ? 1 : 0;
}

HRESULT APIENTRY enumerate_meta_commands(D3D12DDI_HDEVICE, UINT* count, D3D12DDIARG_META_COMMAND_DESC*)
{
    if (!count)
        return E_INVALIDARG;
    *count = 0;   // no meta commands
    return S_OK;
}

D3D12DDI_DRIVER_MATCHING_IDENTIFIER_STATUS APIENTRY check_driver_matching_identifier(
    D3D12DDI_HDEVICE, D3D12DDI_SERIALIZED_DATA_TYPE, const D3D12DDI_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER_0054*)
{
    return D3D12DDI_DRIVER_MATCHING_IDENTIFIER_UNRECOGNIZED;   // we serialize nothing
}

void APIENTRY implicit_shader_cache_control(D3D12DDI_HDEVICE, D3D12DDI_IMPLICIT_SHADER_CACHE_CONTROL_FLAGS_0080)
{
    // No driver-managed shader cache (caps report DriverManagedShaderCachePresent = FALSE).
}

} // namespace

void fill_device_core(D3D12DDI_DEVICE_FUNCS_CORE_0088& t)
{
    t.pfnGetDescriptorSizeInBytes = get_descriptor_size_in_bytes;
    t.pfnCheckMultisampleQualityLevels = check_multisample_quality_levels;
    t.pfnEnumerateMetaCommands = enumerate_meta_commands;
    t.pfnCheckDriverMatchingIdentifier = check_driver_matching_identifier;
    t.pfnImplicitShaderCacheControl = implicit_shader_cache_control;
}

} // namespace vgd::umd
