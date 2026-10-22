// SPDX-License-Identifier: MIT
// D3D12DDI_DEVICE_FUNCS_CORE_0088 slots of the device itself: format queries, root signatures, residency and the
// node map. The other slots are filled by the *_ddi.cpp files; what none fills is a logging stub (stubs.h).
#include "device_core.h"
#include "device.h"
#include "log.h"
#include "objects.h"
#include "vkd3d_bridge.h"

#include <cstddef>
#include <new>

namespace vgd::umd {

namespace {

// Formats the present path can hand to the compositor (present.cpp virgl_format).
bool displayable(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return true;
    default: return false;
    }
}

// The DDI's optional support bits of a format, from the API's (the runtime knows what each feature level requires
// of every format; vkd3d-proton reports the rest the same way the API does).
UINT format_support(ID3D12Device* d, DXGI_FORMAT format)
{
    if (format == DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM)
        return D3D12DDI_FORMAT_SUPPORT_NOT_SUPPORTED;   // the one format allowed to say so
    D3D12_FEATURE_DATA_FORMAT_SUPPORT f{format};
    if (FAILED(d->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &f, sizeof(f))))
        return 0;
    struct Bit {
        UINT api, ddi;
    };
    static const Bit bits1[] = {
        {D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE, D3D12DDI_FORMAT_SUPPORT_SHADER_SAMPLE},
        {D3D12_FORMAT_SUPPORT1_RENDER_TARGET, D3D12DDI_FORMAT_SUPPORT_RENDERTARGET},
        {D3D12_FORMAT_SUPPORT1_BLENDABLE, D3D12DDI_FORMAT_SUPPORT_BLENDABLE},
        {D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET, D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET},
        {D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD, D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD},
        {D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER, D3D12DDI_FORMAT_SUPPORT_VERTEX_BUFFER},
        {D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW, D3D12DDI_FORMAT_SUPPORT_UAV_WRITES},
        {D3D12_FORMAT_SUPPORT1_BUFFER, D3D12DDI_FORMAT_SUPPORT_BUFFER},
        {D3D12_FORMAT_SUPPORT1_SHADER_GATHER, D3D12DDI_FORMAT_SUPPORT_SHADER_GATHER},
    };
    static const Bit bits2[] = {
        {D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD, D3D12DDI_FORMAT_SUPPORT_UAV_READS},
        {D3D12_FORMAT_SUPPORT2_OUTPUT_MERGER_LOGIC_OP, D3D12DDI_FORMAT_SUPPORT_OUTPUT_MERGER_LOGIC_OP},
        {D3D12_FORMAT_SUPPORT2_TILED, D3D12DDI_FORMAT_SUPPORT_TILED},
    };
    UINT s = 0;
    for (const Bit& b : bits1)
        if (f.Support1 & b.api)
            s |= b.ddi;
    for (const Bit& b : bits2)
        if (f.Support2 & b.api)
            s |= b.ddi;
    if ((s & D3D12DDI_FORMAT_SUPPORT_RENDERTARGET) && displayable(format))
        s |= D3D12DDI_FORMAT_SUPPORT_DISPLAY;
    return s;
}

void APIENTRY check_format_support(D3D12DDI_HDEVICE h, DXGI_FORMAT format, UINT* support)
{
    *support = format_support(Device::from(h)->d3d, format);
    VGD_VERBOSE("CheckFormatSupport format=%d -> 0x%x", static_cast<int>(format), *support);
}

// The plane views of depth/stencil formats: sampled, never attachments.
bool depth_stencil_view(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT: return true;
    default: return false;
    }
}

// Whether vkd3d-proton has a format: its colour table (vkd3d_get_vk_format), or its depth/stencil table, which only
// a device's lookup sees. The runtime asks about every DXGI format, and vkd3d-proton logs a FIXME for each it lacks.
bool known_format(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM: return true;
    default: return vgd_vkd3d_vk_format(f) != VK_FORMAT_UNDEFINED;
    }
}

// The depth/stencil plane views report MULTISAMPLE_LOAD but must answer 0 levels above one sample, which the runtime
// checks at device creation (the API's CheckFeatureSupport, which vkd3d-proton answers, allows them). Every other
// format keeps the API's answer: the runtime also checks that a typeless format agrees with its typed members.
void APIENTRY check_multisample_quality_levels(D3D12DDI_HDEVICE h, DXGI_FORMAT format, UINT samples,
                                               D3D12DDI_MULTISAMPLE_QUALITY_LEVEL_FLAGS flags, UINT* levels)
{
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{format, samples,
                                                    static_cast<D3D12_MULTISAMPLE_QUALITY_LEVEL_FLAGS>(flags), 0};
    if ((format != DXGI_FORMAT_UNKNOWN && !known_format(format)) ||
        FAILED(Device::from(h)->d3d->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof(q))) ||
        (samples > 1 && depth_stencil_view(format)))
        q.NumQualityLevels = 0;
    *levels = q.NumQualityLevels;
    VGD_VERBOSE("CheckMultisampleQualityLevels format=%d samples=%u flags=%d -> %u", static_cast<int>(format), samples,
                static_cast<int>(flags), *levels);
}

// One physical adapter, one node: the mask is bit 0 and the node map names physical adapter 0.
UINT APIENTRY get_implicit_physical_adapter_mask(D3D12DDI_HDEVICE)
{
    return 1;
}

void APIENTRY query_node_map(D3D12DDI_HDEVICE, UINT count, UINT* map)
{
    for (UINT i = 0; i < count; i++)
        map[i] = i == 0 ? 0 : D3D12DDI_NODE_MAP_HIDE_NODE;
    VGD_TRACE("QueryNodeMap count=%u", count);
}

UINT APIENTRY get_descriptor_size_in_bytes(D3D12DDI_HDEVICE h, D3D12DDI_DESCRIPTOR_HEAP_TYPE type)
{
    return Device::from(h)->d3d->GetDescriptorHandleIncrementSize(static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(type));
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

// --- Root signatures ---------------------------------------------------------------------------------------
// The DDI's root signature 1.1 structs are the API's D3D12_*1 structs field for field; vkd3d-proton takes the API
// form, serialized.
#define VGD_SAME_LAYOUT(ddi, api, ...)                                                                        \
    static_assert(sizeof(ddi) == sizeof(api) && offsetof(ddi, __VA_ARGS__) == offsetof(api, __VA_ARGS__),   \
                  #ddi " must match " #api)
VGD_SAME_LAYOUT(D3D12DDI_ROOT_SIGNATURE_0013, D3D12_ROOT_SIGNATURE_DESC1, pStaticSamplers);
VGD_SAME_LAYOUT(D3D12DDI_ROOT_SIGNATURE_0013, D3D12_ROOT_SIGNATURE_DESC1, Flags);
VGD_SAME_LAYOUT(D3D12DDI_ROOT_PARAMETER_0013, D3D12_ROOT_PARAMETER1, DescriptorTable);
VGD_SAME_LAYOUT(D3D12DDI_ROOT_PARAMETER_0013, D3D12_ROOT_PARAMETER1, ShaderVisibility);
VGD_SAME_LAYOUT(D3D12DDI_DESCRIPTOR_RANGE_0013, D3D12_DESCRIPTOR_RANGE1, OffsetInDescriptorsFromTableStart);
VGD_SAME_LAYOUT(D3D12DDI_ROOT_DESCRIPTOR_0013, D3D12_ROOT_DESCRIPTOR1, Flags);
VGD_SAME_LAYOUT(D3D12DDI_ROOT_CONSTANTS, D3D12_ROOT_CONSTANTS, Num32BitValues);
VGD_SAME_LAYOUT(D3D12DDI_STATIC_SAMPLER, D3D12_STATIC_SAMPLER_DESC, ShaderVisibility);
#undef VGD_SAME_LAYOUT

SIZE_T APIENTRY calc_private_root_signature_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0013*)
{
    return sizeof(RootSignatureObject);
}

HRESULT APIENTRY create_root_signature(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0013* args,
                                       D3D12DDI_HROOTSIGNATURE hr_)
{
    auto* obj = new (hr_.pDrvPrivate) RootSignatureObject;
    HRESULT hr = E_INVALIDARG;
    if (args->Version == D3D12DDI_ROOT_SIGNATURE_VERSION_1_1 && args->pRootSignature_1_1) {
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{D3D_ROOT_SIGNATURE_VERSION_1_1};
        desc.Desc_1_1 = *reinterpret_cast<const D3D12_ROOT_SIGNATURE_DESC1*>(args->pRootSignature_1_1);
        Com<ID3DBlob> blob;
        hr = vgd_vkd3d_serialize_root_signature(&desc, blob.put_void());
        if (SUCCEEDED(hr))
            hr = Device::from(h)->d3d->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                           __uuidof(ID3D12RootSignature), obj->rs.put_void());
    }
    if (FAILED(hr)) {
        VGD_WARN("CreateRootSignature version=%d failed: 0x%08lx", static_cast<int>(args->Version), hr);
        obj->~RootSignatureObject();
        return E_OUTOFMEMORY;   // creation DDIs may only report E_OUTOFMEMORY (or device removed)
    }
    return S_OK;
}

void APIENTRY destroy_root_signature(D3D12DDI_HDEVICE, D3D12DDI_HROOTSIGNATURE h)
{
    object<RootSignatureObject>(h)->~RootSignatureObject();
}

// --- Residency ---------------------------------------------------------------------------------------------
// Our memory is Vulkan memory owned by Venus; the host driver keeps it resident. MakeResident therefore completes
// immediately (S_OK, no paging fence to wait on) and Evict is a no-op.
HRESULT APIENTRY make_resident(D3D12DDI_HDEVICE, D3D12DDIARG_MAKERESIDENT_0001* args)
{
    args->WaitMask = 0;
    return S_OK;
}

HRESULT APIENTRY evict(D3D12DDI_HDEVICE, const D3D12DDIARG_EVICT*)
{
    return S_OK;
}

} // namespace

void fill_device_core(D3D12DDI_DEVICE_FUNCS_CORE_0088& t)
{
    t.pfnCheckFormatSupport = check_format_support;
    t.pfnGetImplicitPhysicalAdapterMask = get_implicit_physical_adapter_mask;
    t.pfnQueryNodeMap = query_node_map;
    t.pfnGetDescriptorSizeInBytes = get_descriptor_size_in_bytes;
    t.pfnCheckMultisampleQualityLevels = check_multisample_quality_levels;
    t.pfnEnumerateMetaCommands = enumerate_meta_commands;
    t.pfnCheckDriverMatchingIdentifier = check_driver_matching_identifier;
    t.pfnImplicitShaderCacheControl = implicit_shader_cache_control;
    t.pfnCalcPrivateRootSignatureSize = calc_private_root_signature_size;
    t.pfnCreateRootSignature = create_root_signature;
    t.pfnDestroyRootSignature = destroy_root_signature;
    t.pfnMakeResident = make_resident;
    t.pfnEvict = evict;
    fill_pipeline_ddi(t);
    fill_command_ddi(t);
    fill_resource_ddi(t);
    fill_descriptor_ddi(t);
    fill_query_ddi(t);
    fill_indirect_ddi(t);
}

} // namespace vgd::umd
