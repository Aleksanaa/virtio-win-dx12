// SPDX-License-Identifier: MIT
// Shared and presentable surfaces, as both UMDs create and open them. Their host resource is Venus memory:
//   - creating one, the surface is a linear image on exported memory (our ICD build exports an NT handle); we open
//     that handle on our kernel device (kmt.h), which gives an allocation handle of this process, and the runtime
//     allocation is created as an alias of it (viogpu::AllocationAlias, our KMD patch), naming the same host resource;
//   - opening one is the reverse: an NT handle of the opened runtime resource goes to our ICD's dma-buf import
//     (icd/patches/0003, 0005), under a linear image of the same size and format.
// Allocation private data (viogpu::AllocationExchange) carries a virgl format, which the KMD scans out
// (VioGpuVirglFormatToScanout), or a Mesa pipe format where virgl has none.
#pragma once

#include "kmt.h"
#include "viogpu_abi.h"

#include <dxgiformat.h>

namespace vgd::umd {

// The exported memory of a surface being created, opened on our kernel device, and the allocation private data of a
// runtime allocation aliasing it. The memory stays open until this is destroyed, past the AllocateCb that creates the
// alias.
class SurfaceAlias {
public:
    explicit SurfaceAlias(KmtDevice* kmt) : kmt_(kmt) {}
    ~SurfaceAlias();
    SurfaceAlias(const SurfaceAlias&) = delete;
    SurfaceAlias& operator=(const SurfaceAlias&) = delete;

    // Opens `fd` (closing the NT handle it is): `size` bytes under a linear 2D image of `format` (virgl_format) with
    // rows `stride` bytes apart. `display`: a surface that may be scanned out.
    HRESULT open(int fd, ULONG format, UINT width, UINT height, UINT64 size, UINT stride, bool display);
    // The host (virtio-gpu) resource id, for logs: the host names resources by it in its errors.
    UINT host_resource() const;

    viogpu::AllocationAlias data{};   // the allocation private data, once open

private:
    KmtDevice* kmt_;
    D3DKMT_HANDLE opened_ = 0;
};

// The allocation private data of an opened shared resource (`allocations` of them, the first `size` bytes of
// `data`) if it is one we can import: a shareable blob under a single 2D image. Null, logged as `what`, otherwise.
const viogpu::AllocationExchange* importable_surface(const char* what, UINT allocations, const void* data, UINT size,
                                                     D3DKMT_HANDLE allocation, D3DKMT_HANDLE km_resource);
// An fd of the opened runtime resource `km_resource` for our ICD's import, which takes it over (if the import does
// not, the caller closes handle_of_fd(fd)); -1, logged as `what`, on failure.
int import_fd(const char* what, KmtDevice* kmt, D3DKMT_HANDLE km_resource);

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
