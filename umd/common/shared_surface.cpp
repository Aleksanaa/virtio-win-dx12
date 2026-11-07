// SPDX-License-Identifier: MIT
#include "shared_surface.h"
#include "log.h"

namespace venera::umd {

SurfaceAlias::~SurfaceAlias()
{
    if (opened_)
        kmt_->close(opened_);
}

HRESULT SurfaceAlias::open(int fd, ULONG format, UINT width, UINT height, UINT64 size, UINT stride, bool display)
{
    const HANDLE nt = handle_of_fd(fd);
    D3DKMT_HANDLE source = 0;
    HRESULT hr = kmt_ && nt != INVALID_HANDLE_VALUE ? kmt_->open_shared(nt, &opened_, &source) : E_FAIL;
    if (nt != INVALID_HANDLE_VALUE)
        CloseHandle(nt);   // the opened resource keeps the allocation
    if (FAILED(hr)) {
        VENERA_WARN("shared surface: could not open the exported memory (handle %p): 0x%08lx", nt, hr);
        return hr;
    }
    auto& o = data.exchange.options;
    o.target = viogpu::kPipeTexture2d;
    o.format = format;
    o.bind = viogpu::kBindRenderTarget | viogpu::kBindSamplerView | viogpu::kBindShared;
    if (display)
        o.bind |= viogpu::kBindDisplayTarget;
    o.width = width;
    o.height = height;
    o.depth = 1;
    o.array_size = 1;
    data.exchange.Size = size;
    data.exchange.BlobMem = viogpu::kBlobMemHost3d;
    data.exchange.BlobFlags = viogpu::kBlobFlagShareable;
    data.exchange.Stride = stride;
    data.Magic = viogpu::kAllocationAliasMagic;
    data.SourceAllocation = source;
    return S_OK;
}

UINT SurfaceAlias::host_resource() const
{
    return opened_ ? kmt_->resource_id(data.SourceAllocation) : 0;
}

const viogpu::AllocationExchange* importable_surface(const char* what, UINT allocations, const void* data, UINT size,
                                                     D3DKMT_HANDLE allocation, D3DKMT_HANDLE km_resource)
{
    if (allocations != 1 || !data || size < sizeof(viogpu::AllocationExchange)) {
        VENERA_WARN("%s: %u allocations, private data %u bytes: not a surface of ours", what, allocations, size);
        return nullptr;
    }
    const auto* e = static_cast<const viogpu::AllocationExchange*>(data);
    const viogpu::ResourceOptions& o = e->options;
    VENERA_TRACE("%s: allocation 0x%x, km resource 0x%x: target %lu format %lu bind 0x%lx %lux%lux%lu array %lu levels "
                 "%lu samples %lu; size %llu, blob mem %lu, blob flags 0x%lx, stride %lu",
                 what, allocation, km_resource, o.target, o.format, o.bind, o.width, o.height, o.depth, o.array_size,
                 o.last_level + 1, o.nr_samples, static_cast<unsigned long long>(e->Size), e->BlobMem, e->BlobFlags,
                 e->Stride);
    if (o.target != viogpu::kPipeTexture2d || o.depth > 1 || o.array_size > 1 || o.last_level || o.nr_samples > 1 ||
        e->BlobMem != viogpu::kBlobMemHost3d || !(e->BlobFlags & viogpu::kBlobFlagShareable)) {
        VENERA_WARN("%s: only shareable blobs under a single 2D image can be opened", what);
        return nullptr;
    }
    return e;
}

int import_fd(const char* what, KmtDevice* kmt, D3DKMT_HANDLE allocation,
              const viogpu::AllocationExchange& surface)
{
    HANDLE nt = nullptr;
    if (!kmt || FAILED(kmt->share_surface(allocation, surface, &nt))) {
        VENERA_WARN("%s: could not export allocation 0x%x", what, allocation);
        return -1;
    }
    // For the log: the host names resources by this id in its errors (an import of one it does not know kills the
    // Venus context).
    D3DKMT_HANDLE opened = 0, source = 0;
    if (SUCCEEDED(kmt->open_shared(nt, &opened, &source))) {
        VENERA_TRACE("%s: allocation 0x%x is host resource %u", what, allocation, kmt->resource_id(source));
        kmt->close(opened);
    }
    const int fd = fd_of_handle(nt);
    if (fd < 0)
        CloseHandle(nt);
    return fd;
}

} // namespace venera::umd
