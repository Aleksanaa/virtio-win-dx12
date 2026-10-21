// SPDX-License-Identifier: MIT
// D3D12DDI_DEVICE_FUNCS_CORE_0088 slots we implement. Everything else in the table is a logging stub (stubs.h).
#include "device_core.h"
#include "device.h"
#include "log.h"
#include "objects.h"

#include <vgd/root_signature.h>

#include <cstddef>
#include <memory>
#include <new>

namespace vgd::umd {

namespace {

// How D3D lets a DXGI format be used. Depth/stencil formats share one Vulkan format with their plane views, but
// D3D allows different things on each: the DSV format is only an attachment, its views are only sampled.
enum class FormatClass { color, depth_stencil, depth_view, stencil_view, depth_typeless };

FormatClass classify(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_D16_UNORM: return FormatClass::depth_stencil;
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return FormatClass::depth_view;
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT: return FormatClass::stencil_view;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R24G8_TYPELESS: return FormatClass::depth_typeless;
    default: return FormatClass::color;
    }
}

// The optional support bits for a format. The runtime knows what each feature level requires; the driver reports
// what the format can do beyond that, from the Vulkan features of the format it is stored as.
UINT format_support(Device& d, DXGI_FORMAT format, VkSampleCountFlags* samples_out = nullptr)
{
    if (format == DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM)
        return D3D12DDI_FORMAT_SUPPORT_NOT_SUPPORTED;   // the one format allowed to say so
    const vgd::FormatFeatures f = d.core->format_features(format);
    if (samples_out)
        *samples_out = f.samples;
    const bool sampled = f.optimal & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT;
    const bool msaa = f.samples & ~VK_SAMPLE_COUNT_1_BIT;
    UINT s = 0;
    switch (classify(format)) {
    case FormatClass::depth_stencil:
        if (msaa && (f.optimal & VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT))
            s |= D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET;
        return s;
    case FormatClass::depth_view:
        if (f.optimal & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
            s |= D3D12DDI_FORMAT_SUPPORT_SHADER_SAMPLE;
        if (sampled)
            s |= D3D12DDI_FORMAT_SUPPORT_SHADER_GATHER;
        if (msaa && sampled)
            s |= D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD;
        return s;
    case FormatClass::stencil_view:
        if (msaa && sampled)
            s |= D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD;
        return s;
    case FormatClass::depth_typeless:
        if (msaa)
            s |= D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET;
        if (msaa && sampled)
            s |= D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD;
        return s;
    case FormatClass::color:
        break;
    }

    if (f.optimal & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
        s |= D3D12DDI_FORMAT_SUPPORT_SHADER_SAMPLE;
    if (sampled)
        s |= D3D12DDI_FORMAT_SUPPORT_SHADER_GATHER;
    if (f.optimal & VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT)
        s |= D3D12DDI_FORMAT_SUPPORT_RENDERTARGET;
    if (f.optimal & VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BLEND_BIT)
        s |= D3D12DDI_FORMAT_SUPPORT_BLENDABLE;
    if (f.optimal & VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT)
        s |= D3D12DDI_FORMAT_SUPPORT_UAV_WRITES;
    if (f.optimal & VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT)
        s |= D3D12DDI_FORMAT_SUPPORT_UAV_READS;
    if (f.buffer & VK_FORMAT_FEATURE_2_UNIFORM_TEXEL_BUFFER_BIT)
        s |= D3D12DDI_FORMAT_SUPPORT_BUFFER;
    if (f.buffer & VK_FORMAT_FEATURE_2_VERTEX_BUFFER_BIT)
        s |= D3D12DDI_FORMAT_SUPPORT_VERTEX_BUFFER;
    if (msaa) {
        if (s & D3D12DDI_FORMAT_SUPPORT_RENDERTARGET)
            s |= D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET;
        if (sampled)
            s |= D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD;
    }
    // Swapchain formats DWM composes from; present copies into the scanout allocation.
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        if (s & D3D12DDI_FORMAT_SUPPORT_RENDERTARGET)
            s |= D3D12DDI_FORMAT_SUPPORT_DISPLAY;
        break;
    default:
        break;
    }
    // Not reported yet: tiled (no tiled resources), logic op (caps say no).
    return s;
}

void APIENTRY check_format_support(D3D12DDI_HDEVICE h, DXGI_FORMAT format, UINT* support)
{
    *support = format_support(*Device::from(h), format);
    VGD_VERBOSE("CheckFormatSupport format=%d -> 0x%x", static_cast<int>(format), *support);
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

UINT APIENTRY get_descriptor_size_in_bytes(D3D12DDI_HDEVICE, D3D12DDI_DESCRIPTOR_HEAP_TYPE)
{
    return kDescriptorSize;
}

// One quality level (the standard pattern) per supported sample count, consistent with format_support(): a count
// above 1 is offered only for formats that can be multisampled attachments (MULTISAMPLE_RENDERTARGET). View-only
// formats (e.g. R32_FLOAT_X8X24_TYPELESS) report MULTISAMPLE_LOAD but must answer 0 levels: the runtime checks.
void APIENTRY check_multisample_quality_levels(D3D12DDI_HDEVICE h, DXGI_FORMAT format, UINT samples,
                                               D3D12DDI_MULTISAMPLE_QUALITY_LEVEL_FLAGS flags, UINT* levels)
{
    *levels = 0;
    if (flags & D3D12DDI_MULTISAMPLE_QUALITY_LEVEL_FLAG_TILED_RESOURCE)
        return;   // no tiled resources
    if (format == DXGI_FORMAT_UNKNOWN) {
        *levels = samples == 1 ? 1 : 0;
    } else if (samples && !(samples & (samples - 1))) {
        VkSampleCountFlags counts = 0;
        const UINT s = format_support(*Device::from(h), format, &counts);
        const UINT ms = D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET;
        if ((counts & samples) && (samples == 1 || (s & ms)))
            *levels = 1;
    }
    VGD_VERBOSE("CheckMultisampleQualityLevels format=%d samples=%u flags=%d -> %u", static_cast<int>(format), samples,
                static_cast<int>(flags), *levels);
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
// The DDI's root signature 1.1 structs are the API's D3D12_*1 structs field for field; core takes the API form.
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

HRESULT APIENTRY create_root_signature(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0013* args,
                                       D3D12DDI_HROOTSIGNATURE h)
{
    HRESULT hr = E_INVALIDARG;
    std::unique_ptr<vgd::RootSignature> rs;
    if (args->Version == D3D12DDI_ROOT_SIGNATURE_VERSION_1_1 && args->pRootSignature_1_1)
        hr = vgd::RootSignature::create(*reinterpret_cast<const D3D12_ROOT_SIGNATURE_DESC1*>(args->pRootSignature_1_1),
                                        &rs);
    auto* obj = new (h.pDrvPrivate) RootSignatureObject{std::move(rs)};
    if (FAILED(hr)) {
        VGD_WARN("CreateRootSignature version=%d failed: 0x%08lx", static_cast<int>(args->Version), hr);
        obj->~RootSignatureObject();
        return E_OUTOFMEMORY;   // creation DDIs may only report E_OUTOFMEMORY (or device removed)
    }
    VGD_VERBOSE("CreateRootSignature: %u parameters, %u static samplers, root data %u bytes",
                obj->rs->desc().NumParameters, obj->rs->desc().NumStaticSamplers, obj->rs->root_data_size());
    return S_OK;
}

void APIENTRY destroy_root_signature(D3D12DDI_HDEVICE, D3D12DDI_HROOTSIGNATURE h)
{
    static_cast<RootSignatureObject*>(h.pDrvPrivate)->~RootSignatureObject();
}

// --- Residency ---------------------------------------------------------------------------------------------
// Our memory is Vulkan memory owned by Venus; the host driver keeps it resident. MakeResident therefore completes
// immediately (S_OK, no paging fence to wait on) and Evict is a no-op.
HRESULT APIENTRY make_resident(D3D12DDI_HDEVICE, D3D12DDIARG_MAKERESIDENT_0001* args)
{
    VGD_VERBOSE("MakeResident: %u objects, first type %d", args->NumObjects,
                args->NumObjects ? static_cast<int>(args->pObjects[0].Type) : -1);
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
