// SPDX-License-Identifier: MIT
// A D3D12 texture on memory shared with another process (linear_image.h): vkd3d-proton cannot place a texture on
// such memory, so the image is created on its device and lent to a resource of its
// (CreateResourceFromBorrowedHandle).
#pragma once

#include "engine.h"
#include "linear_image.h"

#include <memory>

namespace vgd::umd {

class SharedImage {
public:
    ~SharedImage();

    // A single-mip 2D colour texture of `desc` on newly exported memory; *fd as LinearImage::create_exported.
    static HRESULT create_exported(const std::shared_ptr<Engine>& engine, const D3D12_RESOURCE_DESC& desc,
                                   const DXGI_FORMAT* castable, UINT castable_count, std::unique_ptr<SharedImage>* out,
                                   int* fd);
    // The reverse: a texture of `desc` on `size` bytes another process exported; fd and *consumed as
    // LinearImage::create_imported. The contents are kept in practice (see lend()).
    static HRESULT create_imported(const std::shared_ptr<Engine>& engine, const D3D12_RESOURCE_DESC& desc,
                                   const DXGI_FORMAT* castable, UINT castable_count, int fd, uint64_t size,
                                   std::unique_ptr<SharedImage>* out, bool* consumed);

    ID3D12Resource* resource() const { return resource_.get(); }
    uint64_t size() const { return image_->size(); }
    uint32_t row_pitch() const { return image_->row_pitch(); }

private:
    static HRESULT describe(const D3D12_RESOURCE_DESC& desc, const DXGI_FORMAT* castable, UINT castable_count,
                            LinearImageDesc* out);
    HRESULT lend(const D3D12_RESOURCE_DESC& desc);

    std::shared_ptr<Engine> engine_;
    std::unique_ptr<LinearImage> image_;
    Com<ID3D12Resource> resource_;   // borrows image_: declared after it, so released first
};

} // namespace vgd::umd
