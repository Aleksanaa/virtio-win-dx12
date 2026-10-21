// SPDX-License-Identifier: MIT
// Plan Phase 3 milestone 5: presentable textures. For each swapchain format, a linear texture on exported memory is
// rendered to through its RTV (clear, then a clear of a rectangle, through the sRGB view where there is one), and
// read back three times: through D3D12's copy path, through the exported dma-buf mapped on the CPU at the reported
// row pitch, which is how another process (the compositor) sees it, and through an image importing that dma-buf.
#include "test_util.h"

#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>

namespace {

constexpr uint32_t kWidth = 100, kHeight = 60;   // odd sizes, so the row pitch is padded
constexpr uint32_t kCopyPitch = 1024;            // D3D12 footprints are 256-byte aligned

struct Case {
    const char* name;
    DXGI_FORMAT format;
    DXGI_FORMAT srgb;   // castable view the rectangle is cleared through, or UNKNOWN
    uint32_t bpp;
    uint64_t red, blue;   // the pixels of opaque red and blue, little-endian
};

const Case kCases[] = {
    {"B8G8R8A8", DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, 4, 0xffff0000u, 0xff0000ffu},
    {"R8G8B8A8", DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 4, 0xff0000ffu, 0xffff0000u},
    {"R10G10B10A2", DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_UNKNOWN, 4, 0xc00003ffu, 0xfff00000u},
    {"R16G16B16A16_FLOAT", DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_UNKNOWN, 8, 0x3c00000000003c00ull,
     0x3c003c0000000000ull},
};

bool present_format(vgd::Device& dev, const Case& c)
{
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kWidth;
    td.Height = kHeight;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = c.format;
    td.SampleDesc = {1, 0};
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    std::unique_ptr<vgd::Resource> target, readback;
    vgd::Device::ExportedTexture exported;
    CHECK_HR(dev.create_presentable_texture(td, &c.srgb, c.srgb != DXGI_FORMAT_UNKNOWN ? 1 : 0, &target, &exported));
    CHECK(exported.fd >= 0);
    CHECK(exported.row_pitch >= kWidth * c.bpp);
    CHECK(exported.size >= uint64_t(exported.row_pitch) * kHeight);
    std::fprintf(stderr, "  %s: exported fd %d, %llu bytes, row pitch %u\n", c.name, exported.fd,
                 static_cast<unsigned long long>(exported.size), exported.row_pitch);

    vgd::RenderTargetView rtv, rtv_rect;
    CHECK_HR(dev.create_render_target_view(*target, nullptr, &rtv));
    if (c.srgb != DXGI_FORMAT_UNKNOWN) {
        // An sRGB view must exist on the UNORM image.
        D3D12_RENDER_TARGET_VIEW_DESC srgb{};
        srgb.Format = c.srgb;
        srgb.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        CHECK_HR(dev.create_render_target_view(*target, &srgb, &rtv_rect));
    } else {
        CHECK_HR(dev.create_render_target_view(*target, nullptr, &rtv_rect));
    }
    CHECK_HR(dev.create_committed_buffer(D3D12_HEAP_TYPE_READBACK, kCopyPitch * kHeight, D3D12_RESOURCE_FLAG_NONE,
                                         &readback));

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    std::unique_ptr<vgd::CommandQueue> queue;
    CHECK_HR(dev.create_command_queue(qd, &queue));
    std::unique_ptr<vgd::CommandAllocator> alloc;
    CHECK_HR(dev.create_command_allocator(D3D12_COMMAND_LIST_TYPE_DIRECT, &alloc));
    std::unique_ptr<vgd::CommandList> list;
    CHECK_HR(dev.create_command_list(D3D12_COMMAND_LIST_TYPE_DIRECT, *alloc, &list));
    std::unique_ptr<vgd::Fence> fence;
    CHECK_HR(dev.create_fence(0, &fence));

    const float red[4] = {1, 0, 0, 1}, blue[4] = {0, 0, 1, 1};
    const D3D12_RECT rect{10, 20, 50, 40};
    list->clear_render_target_view(rtv, red, {});
    list->clear_render_target_view(rtv_rect, blue, {&rect, 1});
    // D3D12 requires the transition; the clears must be complete before the copy reads.
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {nullptr, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET,
                          D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->resource_barrier({&barrier, 1});
    vgd::TextureCopyLocation dst, src;
    dst.resource = readback.get();
    dst.is_footprint = true;
    dst.footprint.Footprint = {c.format, kWidth, kHeight, 1, kCopyPitch};
    src.resource = target.get();
    list->copy_texture_region(dst, 0, 0, 0, src, nullptr);
    CHECK_HR(list->close());
    vgd::CommandList* cl = list.get();
    CHECK_HR(queue->execute_command_lists({&cl, 1}, fence.get(), 1));
    CHECK(fence->wait(1, 5'000'000'000ull) == S_OK);

    auto expected = [&](uint32_t x, uint32_t y) -> uint64_t {
        const bool in = x >= 10 && x < 50 && y >= 20 && y < 40;
        return in ? c.blue : c.red;
    };
    auto verify = [&](const char* what, const uint8_t* base, uint32_t pitch) {
        for (uint32_t y = 0; y < kHeight; y++)
            for (uint32_t x = 0; x < kWidth; x++) {
                uint64_t px = 0;
                std::memcpy(&px, base + y * pitch + x * c.bpp, c.bpp);
                if (px != expected(x, y)) {
                    std::fprintf(stderr, "  %s %s: pixel (%u, %u) = 0x%llx, expected 0x%llx\n", c.name, what, x, y,
                                 static_cast<unsigned long long>(px),
                                 static_cast<unsigned long long>(expected(x, y)));
                    return false;
                }
            }
        return true;
    };
    CHECK(verify("copy", static_cast<const uint8_t*>(readback->cpu_pointer()), kCopyPitch));

    // The exporter's view of the same bytes.
    void* map = mmap(nullptr, exported.size, PROT_READ, MAP_SHARED, exported.fd, 0);
    if (map == MAP_FAILED) {
        std::fprintf(stderr, "  dma-buf cannot be mapped here; skipping the CPU check\n");
    } else {
        dma_buf_sync sync{DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
        ioctl(exported.fd, DMA_BUF_IOCTL_SYNC, &sync);
        const bool ok = verify("dma-buf", static_cast<const uint8_t*>(map), exported.row_pitch);
        sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
        ioctl(exported.fd, DMA_BUF_IOCTL_SYNC, &sync);
        munmap(map, exported.size);
        CHECK(ok);
    }

    // The importer's side (as for a texture another driver shares with us): the same memory under an imported
    // image, read through D3D12's copy path. The import takes its fd.
    std::unique_ptr<vgd::Resource> imported;
    bool consumed = false;
    const int dup_fd = dup(exported.fd);
    CHECK(dup_fd >= 0);
    const HRESULT ihr = dev.import_texture(td, &c.srgb, c.srgb != DXGI_FORMAT_UNKNOWN ? 1 : 0, dup_fd, exported.size,
                                           &imported, &consumed);
    if (!consumed)
        close(dup_fd);
    CHECK_HR(ihr);
    std::memset(readback->cpu_pointer(), 0, kCopyPitch * kHeight);
    CHECK_HR(alloc->reset());
    CHECK_HR(list->reset(*alloc));
    src.resource = imported.get();
    list->copy_texture_region(dst, 0, 0, 0, src, nullptr);
    CHECK_HR(list->close());
    CHECK_HR(queue->execute_command_lists({&cl, 1}, fence.get(), 2));
    CHECK(fence->wait(2, 5'000'000'000ull) == S_OK);
    CHECK(verify("import", static_cast<const uint8_t*>(readback->cpu_pointer()), kCopyPitch));
    close(exported.fd);
    return true;
}

bool present(vgd::Device& dev)
{
    if (!dev.adapter().external_memory) {
        std::fprintf(stderr, "  no VK_KHR_external_memory_fd + VK_EXT_external_memory_dma_buf: nothing to test\n");
        return true;
    }
    std::fprintf(stderr, "  images declared external (DRM format modifiers): %s\n",
                 dev.adapter().drm_format_modifier ? "yes" : "no");
    for (const Case& c : kCases)
        if (!present_format(dev, c))
            return false;

    // Only single-mip 2D colour textures are presentable.
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kWidth;
    td.Height = kHeight;
    td.DepthOrArraySize = 1;
    td.MipLevels = 2;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc = {1, 0};
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    std::unique_ptr<vgd::Resource> bad;
    vgd::Device::ExportedTexture exported;
    CHECK(dev.create_presentable_texture(td, nullptr, 0, &bad, &exported) == E_NOTIMPL);
    return true;
}

} // namespace

int main() { return vgd_test::run_on_adapters("present", present); }
