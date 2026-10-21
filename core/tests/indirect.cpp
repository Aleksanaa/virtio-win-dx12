// SPDX-License-Identifier: MIT
// Plan Phase 3 milestone 4, step 3: ExecuteIndirect basics. Draws, indexed draws and dispatches from an argument
// buffer, with and without a count buffer (smaller and larger than the maximum), counted by pipeline statistics
// queries and, for the dispatches, by the shader itself.
#include "test_util.h"

#include <vgd/pipeline.h>

#include <depth_dxil.h>
#include <indirect_dxil.h>

#include <cstdint>

namespace {

constexpr uint32_t kSize = 64, kRowPitch = kSize * 4;

// Argument buffer layout.
constexpr uint32_t kDraws = 0, kDrawStride = 32;              // 3 draws, padded
constexpr uint32_t kIndexed = 128, kIndexedStride = 20;       // 2 indexed draws
constexpr uint32_t kDispatches = 256, kDispatchStride = 12;   // 2 dispatches
constexpr uint32_t kCounts = 512;                             // count values 1 and 5
constexpr uint32_t kArgsSize = 1024;

bool indirect(vgd::Device& dev)
{
    // Graphics: the full-target triangle of the depth tests, without depth.
    D3D12_ROOT_PARAMETER1 params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 1};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {1, 0, 4};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC1 rs_desc{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    std::unique_ptr<vgd::RootSignature> rs_unique;
    CHECK_HR(vgd::RootSignature::create(rs_desc, &rs_unique));
    std::shared_ptr<const vgd::RootSignature> rs = std::move(rs_unique);
    std::shared_ptr<const vgd::Shader> vs, ps, cs;
    CHECK_HR(vgd::Shader::from_code(vgd::ShaderStage::vertex, k_vs_flat, &vs));
    CHECK_HR(vgd::Shader::from_code(vgd::ShaderStage::pixel, k_ps_flat, &ps));
    vgd::GraphicsPipelineDesc g;
    g.root_signature = rs;
    g.vs = vs;
    g.ps = ps;
    auto& rt = g.blend.RenderTarget[0];
    rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_ZERO;
    rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    g.rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    g.rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    g.rasterizer.DepthClipEnable = TRUE;
    g.topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    g.num_render_targets = 1;
    g.rtv_formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    std::unique_ptr<vgd::PipelineState> pso, cpso;
    CHECK_HR(vgd::PipelineState::create_graphics(g, &pso));
    CHECK_HR(pso->compile(dev));

    // Compute: a counter as a root UAV.
    D3D12_ROOT_PARAMETER1 cparam{};
    cparam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    cparam.Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE};
    D3D12_ROOT_SIGNATURE_DESC1 crs_desc{1, &cparam, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    CHECK_HR(vgd::RootSignature::create(crs_desc, &rs_unique));
    std::shared_ptr<const vgd::RootSignature> crs = std::move(rs_unique);
    CHECK_HR(vgd::Shader::from_code(vgd::ShaderStage::compute, k_cs_count, &cs));
    CHECK_HR(vgd::PipelineState::create_compute({crs, cs}, &cpso));
    CHECK_HR(cpso->compile(dev));

    // Signatures.
    D3D12_INDIRECT_ARGUMENT_DESC arg{};
    vgd::CommandSignature draw_sig, indexed_sig, dispatch_sig, bad;
    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    CHECK_HR(vgd::CommandSignature::create({kDrawStride, 1, &arg, 0}, &draw_sig));
    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    CHECK_HR(vgd::CommandSignature::create({kIndexedStride, 1, &arg, 0}, &indexed_sig));
    CHECK(vgd::CommandSignature::create({kIndexedStride - 4, 1, &arg, 0}, &bad) == E_INVALIDARG);
    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    CHECK_HR(vgd::CommandSignature::create({kDispatchStride, 1, &arg, 0}, &dispatch_sig));

    // Arguments, written by the CPU into an upload buffer.
    std::unique_ptr<vgd::Resource> args, indices, counter, target, readback, stats;
    CHECK_HR(dev.create_committed_buffer(D3D12_HEAP_TYPE_UPLOAD, kArgsSize, D3D12_RESOURCE_FLAG_NONE, &args));
    auto* a = static_cast<uint8_t*>(args->cpu_pointer());
    std::memset(a, 0, kArgsSize);
    const D3D12_DRAW_ARGUMENTS draws[3] = {{3, 1, 0, 0}, {3, 2, 0, 0}, {3, 4, 0, 0}};
    for (uint32_t i = 0; i < 3; i++)
        std::memcpy(a + kDraws + i * kDrawStride, &draws[i], sizeof(draws[i]));
    const D3D12_DRAW_INDEXED_ARGUMENTS indexed[2] = {{3, 1, 0, 0, 0}, {3, 2, 0, 0, 0}};
    std::memcpy(a + kIndexed, indexed, sizeof(indexed));
    const D3D12_DISPATCH_ARGUMENTS dispatches[2] = {{2, 1, 1}, {3, 1, 1}};
    std::memcpy(a + kDispatches, dispatches, sizeof(dispatches));
    const uint32_t counts[2] = {1, 5};
    std::memcpy(a + kCounts, counts, sizeof(counts));
    CHECK_HR(dev.create_committed_buffer(D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_FLAG_NONE, &indices));
    const uint16_t index_data[3] = {0, 1, 2};
    std::memcpy(indices->cpu_pointer(), index_data, sizeof(index_data));
    CHECK_HR(dev.create_committed_buffer(D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                         &counter));
    CHECK_HR(dev.create_committed_buffer(D3D12_HEAP_TYPE_READBACK, kRowPitch * kSize + 256, D3D12_RESOURCE_FLAG_NONE,
                                         &readback));
    CHECK_HR(dev.create_committed_buffer(D3D12_HEAP_TYPE_READBACK, 5 * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS),
                                         D3D12_RESOURCE_FLAG_NONE, &stats));
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = td.Height = kSize;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc = {1, 0};
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    CHECK_HR(dev.create_committed_resource(D3D12_HEAP_TYPE_DEFAULT, td, &target));
    vgd::RenderTargetView rtv;
    CHECK_HR(dev.create_render_target_view(*target, nullptr, &rtv));
    std::unique_ptr<vgd::QueryHeap> queries;
    CHECK_HR(dev.create_query_heap({D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, 5, 0}, &queries));

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

    const float clear[4] = {0, 0, 0, 1}, color[4] = {0, 1, 0, 1}, z = 0.5f;
    const D3D12_VIEWPORT viewport{0, 0, kSize, kSize, 0, 1};
    const D3D12_RECT scissor{0, 0, kSize, kSize};
    list->clear_render_target_view(rtv, clear, {});
    list->set_render_targets({&rtv, 1});
    list->set_viewports({&viewport, 1});
    list->set_scissor_rects({&scissor, 1});
    list->set_primitive_topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->set_graphics_root_signature(rs.get());
    list->set_pipeline_state(pso.get());
    list->set_graphics_root_32bit_constants(0, {reinterpret_cast<const uint32_t*>(&z), 1}, 0);
    list->set_graphics_root_32bit_constants(1, {reinterpret_cast<const uint32_t*>(color), 4}, 0);
    const D3D12_INDEX_BUFFER_VIEW ibv{indices->gpu_virtual_address(), 256, DXGI_FORMAT_R16_UINT};
    list->set_index_buffer(&ibv);
    auto counted = [&](uint32_t q, auto&& work) {
        list->begin_query(*queries, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, q);
        work();
        list->end_query(*queries, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, q);
    };
    counted(0, [&] { list->execute_indirect(draw_sig, 3, *args, kDraws, nullptr, 0); });
    counted(1, [&] { list->execute_indirect(draw_sig, 3, *args, kDraws, args.get(), kCounts); });       // count 1
    counted(2, [&] { list->execute_indirect(draw_sig, 2, *args, kDraws, args.get(), kCounts + 4); });   // 5 > 2
    counted(3, [&] { list->execute_indirect(indexed_sig, 2, *args, kIndexed, nullptr, 0); });

    // The counter starts at zero: a fill, through D3D12's buffer copy path.
    list->copy_buffer_region(*counter, 0, *args, kArgsSize - 16, 16);   // zeros
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    list->resource_barrier({&barrier, 1});
    list->set_compute_root_signature(crs.get());
    list->set_pipeline_state(cpso.get());
    list->set_compute_root_descriptor(0, counter->gpu_virtual_address());
    counted(4, [&] { list->execute_indirect(dispatch_sig, 2, *args, kDispatches, nullptr, 0); });
    list->resource_barrier({&barrier, 1});

    list->resolve_query_data(*queries, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 5, *stats, 0);
    list->copy_buffer_region(*readback, kRowPitch * kSize, *counter, 0, 4);
    vgd::TextureCopyLocation dst, src;
    dst.resource = readback.get();
    dst.is_footprint = true;
    dst.footprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, 1, kRowPitch};
    src.resource = target.get();
    list->copy_texture_region(dst, 0, 0, 0, src, nullptr);
    CHECK_HR(list->close());
    vgd::CommandList* cl = list.get();
    CHECK_HR(queue->execute_command_lists({&cl, 1}, fence.get(), 1));
    CHECK(fence->wait(1, 5'000'000'000ull) == S_OK);

    bool ok = true;
    auto expect = [&](const char* what, uint64_t got, uint64_t want) {
        if (got != want) {
            std::fprintf(stderr, "  %s: %llu, expected %llu\n", what, static_cast<unsigned long long>(got),
                         static_cast<unsigned long long>(want));
            ok = false;
        }
    };
    const auto* s = static_cast<const D3D12_QUERY_DATA_PIPELINE_STATISTICS*>(stats->cpu_pointer());
    expect("3 draws: vertices", s[0].IAVertices, 3 * (1 + 2 + 4));
    expect("3 draws: primitives", s[0].IAPrimitives, 1 + 2 + 4);
    expect("count 1: vertices", s[1].IAVertices, 3);
    expect("count 5, max 2: vertices", s[2].IAVertices, 3 * (1 + 2));
    expect("2 indexed draws: vertices", s[3].IAVertices, 3 * (1 + 2));
    expect("2 indexed draws: primitives", s[3].IAPrimitives, 1 + 2);
    expect("2 dispatches: invocations", s[4].CSInvocations, 64 * (2 + 3));
    const auto* px = static_cast<const uint8_t*>(readback->cpu_pointer());
    uint32_t v;
    std::memcpy(&v, px + kRowPitch * kSize, 4);
    expect("2 dispatches: counted", v, 64 * (2 + 3));
    std::memcpy(&v, px + (kSize / 2) * kRowPitch + (kSize / 2) * 4, 4);
    expect("center pixel", v, 0xff00ff00);
    return ok;
}

} // namespace

int main() { return vgd_test::run_on_adapters("indirect", indirect); }
