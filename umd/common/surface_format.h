// SPDX-License-Identifier: MIT
// The formats of shared and presentable surfaces as the KMD knows them: allocation private data
// (viogpu::AllocationExchange) carries a virgl format, which the KMD scans out (VioGpuVirglFormatToScanout), or a
// Mesa pipe format where virgl has none.
#pragma once

#include "viogpu_abi.h"

#include <dxgiformat.h>

namespace vgd::umd {

// The resource private data of the shared surfaces our UMDs create (the allocation's is a viogpu::AllocationAlias),
// which openers get back: what the surface is in API terms. It must stay smaller than the allocation's private data,
// which the KMD reads instead of the larger of the two.
struct SharedSurfaceInfo {
    static constexpr ULONG kMagic = 0x53444756;   // "VGDS"
    ULONG magic = kMagic;
    UINT format = 0;   // DXGI_FORMAT
    UINT width = 0, height = 0;
    UINT bind = 0;     // D3D11_BIND_FLAG

    // The D3D11_BIND_FLAG values both UMDs use (vgd3d12 does not see the D3D11 headers).
    static constexpr UINT kBindShaderResource = 0x8, kBindRenderTarget = 0x20;
};
static_assert(sizeof(SharedSurfaceInfo) < sizeof(viogpu::AllocationAlias));

// The format a surface of `f` is known by; 0 if none.
inline ULONG virgl_format(DXGI_FORMAT f)
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
    case DXGI_FORMAT_A8_UNORM: return viogpu::kVirglFormatA8Unorm;
    case DXGI_FORMAT_R8_UNORM: return viogpu::kVirglFormatR8Unorm;
    default: return 0;
    }
}

// The inverse, with the sRGB sibling for views (UNKNOWN if none); UNKNOWN if none.
inline DXGI_FORMAT dxgi_format(ULONG format, DXGI_FORMAT* srgb)
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
    case viogpu::kVirglFormatA8Unorm: return DXGI_FORMAT_A8_UNORM;
    case viogpu::kVirglFormatR8Unorm: return DXGI_FORMAT_R8_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

} // namespace vgd::umd
