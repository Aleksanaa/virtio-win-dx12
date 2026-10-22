// SPDX-License-Identifier: MIT
// Format support, from DXVK's answers to the API's queries. The DDI's format support bits are its own: each one is
// mapped from the API bit it stands for.
#include "device.h"

namespace vgd::umd {

void APIENTRY check_format_support(D3D10DDI_HDEVICE h, DXGI_FORMAT format, UINT* caps)
{
    Device* d = Device::from(h);
    *caps = 0;
    if (format == DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM) {
        *caps = D3D10_DDI_FORMAT_SUPPORT_NOT_SUPPORTED;
        return;
    }
    UINT api = 0;
    // DXVK fails formats it has no Vulkan format for: they get no support.
    if (FAILED(d->d3d->CheckFormatSupport(format, &api)))
        return;
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 s2{format, 0};
    // DXVK fails SUPPORT2 for formats whose answer is zero; that is just no SUPPORT2 bits.
    if (FAILED(d->d3d->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &s2, sizeof(s2))))
        s2.OutFormatSupport2 = 0;
    const struct {
        UINT api, ddi;
    } map[] = {
        {D3D11_FORMAT_SUPPORT_SHADER_SAMPLE, D3D10_DDI_FORMAT_SUPPORT_SHADER_SAMPLE},
        {D3D11_FORMAT_SUPPORT_RENDER_TARGET, D3D10_DDI_FORMAT_SUPPORT_RENDERTARGET},
        {D3D11_FORMAT_SUPPORT_BLENDABLE, D3D10_DDI_FORMAT_SUPPORT_BLENDABLE},
        {D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET, D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET},
        {D3D11_FORMAT_SUPPORT_MULTISAMPLE_LOAD, D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD},
        {D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER, D3D11_1DDI_FORMAT_SUPPORT_VERTEX_BUFFER},
        {D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW, D3D11_1DDI_FORMAT_SUPPORT_UAV_WRITES},
        {D3D11_FORMAT_SUPPORT_BUFFER, D3D11_1DDI_FORMAT_SUPPORT_BUFFER},
        {D3D11_FORMAT_SUPPORT_SHADER_GATHER, D3D11_1DDI_FORMAT_SUPPORT_SHADER_GATHER},
    };
    for (const auto& m : map)
        if (api & m.api)
            *caps |= m.ddi;
    if (s2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_OUTPUT_MERGER_LOGIC_OP)
        *caps |= D3D11_1DDI_FORMAT_SUPPORT_OUTPUT_MERGER_LOGIC_OP;
    VGD_VERBOSE("CheckFormatSupport(%d): api 0x%x/0x%x -> 0x%x", format, api, s2.OutFormatSupport2, *caps);
}

void APIENTRY check_multisample_quality_levels(D3D10DDI_HDEVICE h, DXGI_FORMAT format, UINT samples, UINT* levels)
{
    Device* d = Device::from(h);
    *levels = 0;
    if (FAILED(d->d3d->CheckMultisampleQualityLevels(format, samples, levels)))
        *levels = 0;
    VGD_VERBOSE("CheckMultisampleQualityLevels(%d, %u) -> %u", format, samples, *levels);
}

} // namespace vgd::umd
