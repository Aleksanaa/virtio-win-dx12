// SPDX-License-Identifier: MIT
// Heaps and placed resources (CreateHeap / CreatePlacedResource): buffers and a texture at offsets in UPLOAD,
// DEFAULT and READBACK heaps, a round trip through them, aliasing, and the placement rules.
#include "test_util.h"

#include <vector>

namespace {

constexpr uint64_t k64K = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
constexpr uint32_t kTexW = 64, kTexH = 128;   // RGBA8: exactly the 32 KiB second half of the buffers
constexpr uint64_t kBytes = 64 * 1024;

D3D12_RESOURCE_DESC buffer_desc(uint64_t size)
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc = {1, 0};
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}

bool placed(vgd::Device& dev)
{
    std::unique_ptr<vgd::Heap> upload_heap, default_heap, readback_heap, buffers_only;
    CHECK_HR(dev.create_heap(D3D12_HEAP_TYPE_UPLOAD, 4 * k64K, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS, &upload_heap));
    CHECK_HR(dev.create_heap(D3D12_HEAP_TYPE_DEFAULT, 16 * k64K, D3D12_HEAP_FLAG_NONE, &default_heap));
    CHECK_HR(dev.create_heap(D3D12_HEAP_TYPE_READBACK, 4 * k64K, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS, &readback_heap));
    CHECK(upload_heap->cpu_pointer() && readback_heap->cpu_pointer() && !default_heap->cpu_pointer());

    // Buffers at 64 KiB in each heap; a CPU-visible one maps at the heap's mapping plus its offset.
    std::unique_ptr<vgd::Resource> upload, gpu, alias, readback;
    CHECK_HR(dev.create_placed_resource(*upload_heap, k64K, buffer_desc(kBytes), &upload));
    CHECK_HR(dev.create_placed_resource(*default_heap, k64K, buffer_desc(kBytes), &gpu));
    CHECK_HR(dev.create_placed_resource(*readback_heap, 2 * k64K, buffer_desc(kBytes), &readback));
    CHECK(upload->cpu_pointer() == static_cast<char*>(upload_heap->cpu_pointer()) + k64K);
    CHECK(readback->cpu_pointer() == static_cast<char*>(readback_heap->cpu_pointer()) + 2 * k64K);
    CHECK(gpu->gpu_virtual_address() != 0 && !gpu->cpu_pointer());
    // An alias of `gpu`: the same memory under another buffer, at the same address.
    CHECK_HR(dev.create_placed_resource(*default_heap, k64K, buffer_desc(kBytes / 2), &alias));

    // A texture after the buffers, at its own alignment.
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kTexW;
    td.Height = kTexH;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc = {1, 0};
    uint64_t tex_size = 0, tex_align = 0;
    CHECK_HR(dev.resource_allocation_info(td, &tex_size, &tex_align));
    const uint64_t tex_offset = (2 * k64K + tex_align - 1) / tex_align * tex_align;
    std::unique_ptr<vgd::Resource> texture;
    CHECK_HR(dev.create_placed_resource(*default_heap, tex_offset, td, &texture));

    // The rules: alignment, the heap's end, textures only in DEFAULT heaps.
    std::unique_ptr<vgd::Resource> bad;
    CHECK(dev.create_placed_resource(*default_heap, 1, td, &bad) == E_INVALIDARG);
    CHECK(dev.create_placed_resource(*upload_heap, 3 * k64K, buffer_desc(2 * k64K), &bad) == E_INVALIDARG);
    CHECK(dev.create_placed_resource(*upload_heap, 0, td, &bad) == E_INVALIDARG);

    auto* src = static_cast<uint32_t*>(upload->cpu_pointer());
    for (uint64_t i = 0; i < kBytes / 4; i++)
        src[i] = static_cast<uint32_t>(i * 2654435761u);
    std::memset(readback->cpu_pointer(), 0, kBytes);

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

    // upload -> gpu; the first half read back through the alias, the second half through the texture.
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {nullptr, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->copy_buffer_region(*gpu, 0, *upload, 0, kBytes);
    list->resource_barrier({&barrier, 1});
    list->copy_buffer_region(*readback, 0, *alias, 0, kBytes / 2);
    vgd::TextureCopyLocation tex, foot;
    tex.resource = texture.get();
    foot.resource = gpu.get();
    foot.is_footprint = true;
    foot.footprint.Offset = kBytes / 2;
    foot.footprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kTexW, kTexH, 1, kTexW * 4};
    list->copy_texture_region(tex, 0, 0, 0, foot, nullptr);   // the second half fills the texture
    list->resource_barrier({&barrier, 1});
    vgd::TextureCopyLocation back;
    back.resource = readback.get();
    back.is_footprint = true;
    back.footprint.Offset = kBytes / 2;
    back.footprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kTexW, kTexH, 1, kTexW * 4};
    list->copy_texture_region(back, 0, 0, 0, tex, nullptr);
    CHECK_HR(list->close());
    vgd::CommandList* cl = list.get();
    CHECK_HR(queue->execute_command_lists({&cl, 1}, fence.get(), 1));
    CHECK(fence->wait(1, 5'000'000'000ull) == S_OK);
    const auto* got = static_cast<const uint32_t*>(readback->cpu_pointer());
    for (uint64_t i = 0; i < kBytes / 4; i++)
        if (got[i] != src[i]) {
            std::fprintf(stderr, "  word %llu: 0x%08x, expected 0x%08x\n", static_cast<unsigned long long>(i), got[i], src[i]);
            return false;
        }
    return true;
}

} // namespace

int main() { return vgd_test::run_on_adapters("placed", placed); }
