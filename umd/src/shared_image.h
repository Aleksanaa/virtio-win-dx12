// SPDX-License-Identifier: MIT
// Textures on memory shared with another process through dma-buf: swapchain buffers the compositor reads, and
// textures yttrium's D3D10/11 UMD shares with us. vkd3d-proton cannot place a texture on such memory, so we create
// the VkImage on its device ourselves and lend it to a resource of its (CreateResourceFromBorrowedHandle).
//
// The layout contract with the other side (yttrium's D3D10 UMD in the compositor and its Vulkan WSI, importing ours
// or exporting its own): a linear image of the same size and format at offset 0 of the memory, whose layout each
// side gets from its own plain linear image (yttrium_venus2_resource.c, "Shared single-sample colour images use
// linear tiling"). Memory that is exported or imported must back an image declared external, and RADV offers
// dma-buf images only with DRM format modifiers; so with VK_EXT_image_drm_format_modifier ours is a LINEAR modifier
// image whose row pitch is given explicitly as that of the plain linear image. Without it, it is a plain linear
// image like yttrium's own shared images (which drivers accept but validation flags).
#pragma once

#include "engine.h"

#include <memory>

namespace vgd::umd {

class SharedImage {
public:
    ~SharedImage();

    // A single-mip 2D colour texture of `desc` on newly exported memory. *fd is the export: our ICD build passes
    // the NT handle of the allocation in place of an fd (icd/patches/0003); the caller owns it.
    static HRESULT create_exported(const std::shared_ptr<Engine>& engine, const D3D12_RESOURCE_DESC& desc,
                                   const DXGI_FORMAT* castable, UINT castable_count, std::unique_ptr<SharedImage>* out,
                                   int* fd);
    // The reverse: a texture of `desc` on `size` bytes another process exported. On success the fd belongs to the
    // image; on failure it is still the caller's unless *consumed. The contents are kept in practice (see finish()).
    static HRESULT create_imported(const std::shared_ptr<Engine>& engine, const D3D12_RESOURCE_DESC& desc,
                                   const DXGI_FORMAT* castable, UINT castable_count, int fd, uint64_t size,
                                   std::unique_ptr<SharedImage>* out, bool* consumed);

    ID3D12Resource* resource() const { return resource_.get(); }
    uint64_t size() const { return size_; }
    uint32_t row_pitch() const { return row_pitch_; }

private:
    struct Plan;
    static HRESULT plan(const std::shared_ptr<Engine>& engine, const D3D12_RESOURCE_DESC& desc,
                        const DXGI_FORMAT* castable, UINT castable_count, bool import, Plan* out);
    HRESULT finish(const D3D12_RESOURCE_DESC& desc, bool declared_external, uint64_t linear_pitch);

    std::shared_ptr<Engine> engine_;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    Com<ID3D12Resource> resource_;
    uint64_t size_ = 0;
    uint32_t row_pitch_ = 0;
};

} // namespace vgd::umd
