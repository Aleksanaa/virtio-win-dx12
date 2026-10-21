// SPDX-License-Identifier: MIT
// Plan Phase 3 milestone 4, step 1: compute and UAVs, on a COMPUTE queue. UAV clears of a structured buffer, a typed
// buffer and a texture, then a dispatch writing all three through a root UAV and a descriptor table, with root
// constants. Exact values, including what the dispatch must leave alone.
#include "test_util.h"

#include <vgd/pipeline.h>

#include <compute_dxil.h>

#include <cstdint>

namespace {

constexpr uint32_t kElements = 512, kWritten = 256, kImage = 32, kRowPitch = 256;

uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    return r | g << 8 | b << 16 | a << 24;
}

bool compute(vgd::Device& dev)
{
    if (!dev.adapter().bindless)
        return false;

    D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 1, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0};
    D3D12_ROOT_PARAMETER1 params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 2};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE};
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable = {1, &range};
    D3D12_ROOT_SIGNATURE_DESC1 rs_desc{3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    std::unique_ptr<vgd::RootSignature> rs_unique;
    CHECK_HR(vgd::RootSignature::create(rs_desc, &rs_unique));
    std::shared_ptr<const vgd::RootSignature> rs = std::move(rs_unique);
    std::shared_ptr<const vgd::Shader> cs;
    CHECK_HR(vgd::Shader::from_code(vgd::ShaderStage::compute, k_cs_fill, &cs));
    std::unique_ptr<vgd::PipelineState> pso;
    CHECK_HR(vgd::PipelineState::create_compute({rs, cs}, &pso));
    CHECK_HR(pso->compile(dev));

    std::unique_ptr<vgd::Resource> structured, typed, image, rb_structured, rb_typed, rb_image;
    CHECK_HR(dev.create_committed_buffer(D3D12_HEAP_TYPE_DEFAULT, kElements * 4,
                                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &structured));
    CHECK_HR(dev.create_committed_buffer(D3D12_HEAP_TYPE_DEFAULT, kElements * 4,
                                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &typed));
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = td.Height = kImage;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc = {1, 0};
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    CHECK_HR(dev.create_committed_resource(D3D12_HEAP_TYPE_DEFAULT, td, &image));
    CHECK_HR(dev.create_committed_buffer(D3D12_HEAP_TYPE_READBACK, kElements * 4, D3D12_RESOURCE_FLAG_NONE,
                                         &rb_structured));
    CHECK_HR(dev.create_committed_buffer(D3D12_HEAP_TYPE_READBACK, kElements * 4, D3D12_RESOURCE_FLAG_NONE, &rb_typed));
    CHECK_HR(dev.create_committed_buffer(D3D12_HEAP_TYPE_READBACK, kRowPitch * kImage, D3D12_RESOURCE_FLAG_NONE,
                                         &rb_image));

    // UAVs: the table (typed buffer, texture) in the shader-visible heap at slots 1-2; CPU copies for the clears,
    // which take a CPU descriptor as D3D12's do.
    std::unique_ptr<vgd::DescriptorHeap> gpu_heap, cpu_heap;
    CHECK_HR(dev.create_descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, true, &gpu_heap));
    CHECK_HR(dev.create_descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, false, &cpu_heap));
    vgd::Descriptor d;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Format = DXGI_FORMAT_R32_UINT;
    uav.Buffer = {0, kElements, 0, 0, D3D12_BUFFER_UAV_FLAG_NONE};
    CHECK_HR(dev.create_unordered_access_view(typed.get(), &uav, &d));
    dev.write_descriptor(cpu_heap->cpu_start() + 1, d);
    CHECK_HR(dev.create_unordered_access_view(image.get(), nullptr, &d));
    dev.write_descriptor(cpu_heap->cpu_start() + 2, d);
    dev.copy_descriptors(gpu_heap->cpu_start() + 1, cpu_heap->cpu_start() + 1, 2);
    uav.Format = DXGI_FORMAT_UNKNOWN;
    uav.Buffer.StructureByteStride = 4;
    CHECK_HR(dev.create_unordered_access_view(structured.get(), &uav, &d));
    dev.write_descriptor(cpu_heap->cpu_start() + 0, d);

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    std::unique_ptr<vgd::CommandQueue> queue;
    CHECK_HR(dev.create_command_queue(qd, &queue));
    std::unique_ptr<vgd::CommandAllocator> alloc;
    CHECK_HR(dev.create_command_allocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, &alloc));
    std::unique_ptr<vgd::CommandList> list;
    CHECK_HR(dev.create_command_list(D3D12_COMMAND_LIST_TYPE_COMPUTE, *alloc, &list));
    std::unique_ptr<vgd::Fence> fence;
    CHECK_HR(dev.create_fence(0, &fence));

    const uint32_t beef[4] = {0xdeadbeef, 0, 0, 0}, seven[4] = {7, 0, 0, 0};
    const float blue[4] = {0, 0, 1, 1};
    list->clear_unordered_access_view_uint(cpu_heap->cpu_start()[0], beef);
    list->clear_unordered_access_view_uint(cpu_heap->cpu_start()[1], seven);
    list->clear_unordered_access_view_float(cpu_heap->cpu_start()[2], blue);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    list->resource_barrier({&barrier, 1});

    vgd::DescriptorHeap* heaps[] = {gpu_heap.get()};
    list->set_descriptor_heaps(heaps);
    list->set_compute_root_signature(rs.get());
    list->set_pipeline_state(pso.get());
    const uint32_t params_data[2] = {3, 5};
    list->set_compute_root_32bit_constants(0, params_data, 0);
    list->set_compute_root_descriptor(1, structured->gpu_virtual_address());
    list->set_compute_root_descriptor_table(2, gpu_heap->gpu_start() + vgd::kDescriptorStride);
    list->dispatch(kWritten / 64, 1, 1);
    list->resource_barrier({&barrier, 1});

    list->copy_buffer_region(*rb_structured, 0, *structured, 0, kElements * 4);
    list->copy_buffer_region(*rb_typed, 0, *typed, 0, kElements * 4);
    vgd::TextureCopyLocation dst, src;
    dst.resource = rb_image.get();
    dst.is_footprint = true;
    dst.footprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kImage, kImage, 1, kRowPitch};
    src.resource = image.get();
    list->copy_texture_region(dst, 0, 0, 0, src, nullptr);
    CHECK_HR(list->close());
    vgd::CommandList* cl = list.get();
    CHECK_HR(queue->execute_command_lists({&cl, 1}, fence.get(), 1));
    CHECK(fence->wait(1, 5'000'000'000ull) == S_OK);

    bool ok = true;
    auto expect = [&](const char* what, uint32_t i, uint32_t got, uint32_t want) {
        if (got != want && ok) {
            std::fprintf(stderr, "  %s [%u]: 0x%08x, expected 0x%08x\n", what, i, got, want);
            ok = false;
        }
    };
    const auto* s = static_cast<const uint32_t*>(rb_structured->cpu_pointer());
    const auto* t = static_cast<const uint32_t*>(rb_typed->cpu_pointer());
    for (uint32_t i = 0; i < kElements; i++) {
        expect("root UAV (structured)", i, s[i], i < kWritten ? i * 3 + 5 : 0xdeadbeef);
        expect("table UAV (typed)", i, t[i], i < kWritten ? i + 1000 : 7);
    }
    const auto* img = static_cast<const uint8_t*>(rb_image->cpu_pointer());
    for (uint32_t y = 0; y < kImage; y++)
        for (uint32_t x = 0; x < kImage; x++) {
            uint32_t v;
            std::memcpy(&v, img + y * kRowPitch + x * 4, 4);
            expect("table UAV (texture)", y * kImage + x, v,
                   x < 16 && y < 16 ? rgba(x * 17, y * 17, 51, 255) : rgba(0, 0, 255, 255));
        }
    return ok;
}

} // namespace

int main() { return vgd_test::run_on_adapters("compute", compute); }
