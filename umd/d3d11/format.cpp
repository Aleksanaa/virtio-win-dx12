// SPDX-License-Identifier: MIT
// Format support, from DXVK's answers to the API's queries, held to what the runtime checks at device creation:
// it removes a device whose answers break its rules. The rules are those yttrium's d3d10umd answers by
// (third_party/virtio-win-mesa, frontends/d3d10umd/Device.cpp), which pass the Windows 11 runtime:
//   - formats the driver has no Vulkan format for are NOT_SUPPORTED; R10G10B10_XR_BIAS_A2 reports nothing (only
//     creation is needed);
//   - typeless and depth/stencil formats are no render targets, blend, logic op or typed UAV formats;
//   - the formats a feature level requires to be blendable render targets report both, and only those blend;
//   - logic ops only on the UINT render target formats;
//   - MSAA at 2, 4 and 8 samples with one quality level; depth plane-only views multisample-load but report no
//     quality levels, and the depth storage formats and their typed view siblings must agree.
#include "device.h"

namespace vgd::umd {

namespace {

bool typeless(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC7_TYPELESS: return true;
    default: return false;
    }
}

// Depth/stencil storage: the typeless storage formats and the depth formats.
bool depth_storage(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return true;
    default: return false;
    }
}

// The depth planes as shader resource views: multisample loads, no quality levels.
bool depth_load_only(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT: return true;
    default: return false;
    }
}

bool depth_format(DXGI_FORMAT f)
{
    return depth_storage(f) || depth_load_only(f);
}

// The feature level from which a format must be a blendable render target (0 if never).
D3D_FEATURE_LEVEL blend_level(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8_SNORM: return D3D_FEATURE_LEVEL_10_1;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R16_FLOAT: return D3D_FEATURE_LEVEL_10_0;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return D3D_FEATURE_LEVEL_9_3;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_A8_UNORM:
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return D3D_FEATURE_LEVEL_9_1;
    default: return D3D_FEATURE_LEVEL(0);
    }
}

bool logic_op_format(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R8_UINT: return true;
    default: return false;
    }
}

bool msaa_sample_count(UINT n)
{
    return n == 2 || n == 4 || n == 8;
}

// DXVK's quality levels for `samples` of `format`, at most one.
UINT msaa_levels(Device& d, DXGI_FORMAT format, UINT samples)
{
    UINT levels = 0;
    if (!msaa_sample_count(samples) || depth_load_only(format) ||
        FAILED(d.d3d->CheckMultisampleQualityLevels(format, samples, &levels)))
        return 0;
    return levels ? 1 : 0;
}

bool msaa_any(Device& d, DXGI_FORMAT format)
{
    return msaa_levels(d, format, 2) || msaa_levels(d, format, 4) || msaa_levels(d, format, 8);
}

} // namespace

void APIENTRY check_format_support(D3D10DDI_HDEVICE h, DXGI_FORMAT format, UINT* caps)
{
    Device* d = Device::from(h);
    *caps = 0;
    if (format == DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM)
        return;
    UINT api = 0;
    if (FAILED(d->d3d->CheckFormatSupport(format, &api))) {
        *caps = D3D10_DDI_FORMAT_SUPPORT_NOT_SUPPORTED;
        VGD_VERBOSE("CheckFormatSupport(%d): not supported", format);
        return;
    }
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 s2{format, 0};
    // DXVK fails SUPPORT2 for formats whose answer is zero; that is just no SUPPORT2 bits.
    if (FAILED(d->d3d->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &s2, sizeof(s2))))
        s2.OutFormatSupport2 = 0;
    const bool colour = !typeless(format) && !depth_format(format);
    if (api & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE)
        *caps |= D3D10_DDI_FORMAT_SUPPORT_SHADER_SAMPLE;
    if (api & D3D11_FORMAT_SUPPORT_SHADER_GATHER)
        *caps |= D3D11_1DDI_FORMAT_SUPPORT_SHADER_GATHER;
    if (api & D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER)
        *caps |= D3D11_1DDI_FORMAT_SUPPORT_VERTEX_BUFFER;
    if (api & D3D11_FORMAT_SUPPORT_BUFFER)
        *caps |= D3D11_1DDI_FORMAT_SUPPORT_BUFFER;
    if (colour) {
        if (api & D3D11_FORMAT_SUPPORT_RENDER_TARGET)
            *caps |= D3D10_DDI_FORMAT_SUPPORT_RENDERTARGET;
        const D3D_FEATURE_LEVEL level = blend_level(format);
        if (level && d->d3d->GetFeatureLevel() >= level)
            *caps |= D3D10_DDI_FORMAT_SUPPORT_RENDERTARGET | D3D10_DDI_FORMAT_SUPPORT_BLENDABLE;
        if ((*caps & D3D10_DDI_FORMAT_SUPPORT_RENDERTARGET) && logic_op_format(format) &&
            (s2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_OUTPUT_MERGER_LOGIC_OP))
            *caps |= D3D11_1DDI_FORMAT_SUPPORT_OUTPUT_MERGER_LOGIC_OP;
        if (api & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW)
            *caps |= D3D11_1DDI_FORMAT_SUPPORT_UAV_WRITES;
        if (msaa_any(*d, format)) {
            if (*caps & D3D10_DDI_FORMAT_SUPPORT_RENDERTARGET)
                *caps |= D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET;
            if (*caps & D3D10_DDI_FORMAT_SUPPORT_SHADER_SAMPLE)
                *caps |= D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD;
        }
    } else if (depth_format(format)) {
        const DXGI_FORMAT storage = format;
        if (depth_storage(storage) && msaa_any(*d, storage))
            *caps |= D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET | D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD;
        if (depth_load_only(format) && (api & D3D11_FORMAT_SUPPORT_MULTISAMPLE_LOAD))
            *caps |= D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD;
    }
    VGD_VERBOSE("CheckFormatSupport(%d): api 0x%x/0x%x -> 0x%x", format, api, s2.OutFormatSupport2, *caps);
}

void APIENTRY check_multisample_quality_levels(D3D10DDI_HDEVICE h, DXGI_FORMAT format, UINT samples, UINT* levels)
{
    Device* d = Device::from(h);
    *levels = msaa_levels(*d, format, samples);
    VGD_VERBOSE("CheckMultisampleQualityLevels(%d, %u) -> %u", format, samples, *levels);
}

} // namespace vgd::umd
