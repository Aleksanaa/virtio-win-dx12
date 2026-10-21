// SPDX-License-Identifier: MIT
// Plan Phase 3 milestone 4, step 2: queries. Occlusion and binary occlusion counts of scissored draws that pass or
// fail the depth test, a query never used, pipeline statistics of one draw, and timestamps: rescaled to another
// frequency exactly as the GPU's ticks say, and agreeing with the CPU across a sleep between two submissions.
#include "test_util.h"

#include <vgd/pipeline.h>

#include <depth_dxil.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>

namespace {

constexpr uint32_t kSize = 64;
constexpr uint64_t kFrequency = 10'000'000;   // what the Windows KMD reports: the guest's QueryPerformanceFrequency

// Resolve buffer layout, in 64-bit results.
constexpr uint32_t kOcclusion = 0;     // 5 occlusion heap queries
constexpr uint32_t kStatistics = 8;    // 11 statistics
constexpr uint32_t kRaw = 20;          // 2 timestamps in GPU ticks
constexpr uint32_t kScaled = 24;       // the same 2 at kFrequency
constexpr uint32_t kLater = 28;        // 3 timestamps at kFrequency, from the second submission
constexpr uint32_t kResults = 32;

bool run(vgd::CommandQueue& queue, vgd::CommandList& list, vgd::Fence& fence, uint64_t value)
{
    CHECK_HR(list.close());
    vgd::CommandList* cl = &list;
    CHECK_HR(queue.execute_command_lists({&cl, 1}, &fence, value));
    CHECK(fence.wait(value, 5'000'000'000ull) == S_OK);
    return true;
}

bool queries(vgd::Device& dev)
{
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
    std::shared_ptr<const vgd::Shader> vs, ps;
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
    g.dsv_format = DXGI_FORMAT_D32_FLOAT;
    g.depth_stencil.DepthEnable = TRUE;
    g.depth_stencil.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    g.depth_stencil.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    std::unique_ptr<vgd::PipelineState> pso;
    CHECK_HR(vgd::PipelineState::create_graphics(g, &pso));
    CHECK_HR(pso->compile(dev));

    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = td.Height = kSize;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc = {1, 0};
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    std::unique_ptr<vgd::Resource> target, depth, results;
    CHECK_HR(dev.create_committed_resource(D3D12_HEAP_TYPE_DEFAULT, td, &target));
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    CHECK_HR(dev.create_committed_resource(D3D12_HEAP_TYPE_DEFAULT, td, &depth));
    CHECK_HR(dev.create_committed_buffer(D3D12_HEAP_TYPE_READBACK, kResults * 8, D3D12_RESOURCE_FLAG_NONE, &results));
    std::memset(results->cpu_pointer(), 0xcd, kResults * 8);
    vgd::RenderTargetView rtv;
    vgd::DepthStencilView dsv;
    CHECK_HR(dev.create_render_target_view(*target, nullptr, &rtv));
    CHECK_HR(dev.create_depth_stencil_view(*depth, nullptr, &dsv));

    std::unique_ptr<vgd::QueryHeap> occlusion, statistics, timestamps;
    CHECK_HR(dev.create_query_heap({D3D12_QUERY_HEAP_TYPE_OCCLUSION, 5, 0}, &occlusion));
    CHECK_HR(dev.create_query_heap({D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, 1, 0}, &statistics));
    CHECK_HR(dev.create_query_heap({D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 3, 0}, &timestamps));

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

    const float color[4] = {1, 0, 0, 1};
    const D3D12_VIEWPORT viewport{0, 0, kSize, kSize, 0, 1};
    const D3D12_RECT scissor{8, 8, 24, 16};   // 16 x 8 = 128 samples
    auto draw = [&](float z) {
        list->set_graphics_root_32bit_constants(0, {reinterpret_cast<const uint32_t*>(&z), 1}, 0);
        list->set_graphics_root_32bit_constants(1, {reinterpret_cast<const uint32_t*>(color), 4}, 0);
        list->draw_instanced(3, 1, 0, 0);
    };

    list->end_query(*timestamps, D3D12_QUERY_TYPE_TIMESTAMP, 0);
    list->clear_depth_stencil_view(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.5f, 0, {});
    list->set_render_targets({&rtv, 1}, &dsv);
    list->set_viewports({&viewport, 1});
    list->set_scissor_rects({&scissor, 1});
    list->set_primitive_topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->set_graphics_root_signature(rs.get());
    list->set_pipeline_state(pso.get());
    const struct {
        D3D12_QUERY_TYPE type;
        float z;
    } occlusion_draws[4] = {
        {D3D12_QUERY_TYPE_OCCLUSION, 0.25f},          // passes: 128
        {D3D12_QUERY_TYPE_BINARY_OCCLUSION, 0.25f},   // 1
        {D3D12_QUERY_TYPE_BINARY_OCCLUSION, 0.75f},   // fails: 0
        {D3D12_QUERY_TYPE_OCCLUSION, 0.75f},          // 0
    };
    for (uint32_t i = 0; i < 4; i++) {
        list->begin_query(*occlusion, occlusion_draws[i].type, i);
        draw(occlusion_draws[i].z);
        list->end_query(*occlusion, occlusion_draws[i].type, i);
    }
    list->begin_query(*statistics, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    draw(0.25f);
    list->end_query(*statistics, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    list->end_query(*timestamps, D3D12_QUERY_TYPE_TIMESTAMP, 1);

    // Query 4 of the occlusion heap was never used.
    list->resolve_query_data(*occlusion, D3D12_QUERY_TYPE_OCCLUSION, 0, 1, *results, kOcclusion * 8);
    list->resolve_query_data(*occlusion, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 1, 2, *results, (kOcclusion + 1) * 8);
    list->resolve_query_data(*occlusion, D3D12_QUERY_TYPE_OCCLUSION, 3, 2, *results, (kOcclusion + 3) * 8);
    list->resolve_query_data(*statistics, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, *results, kStatistics * 8);
    list->resolve_query_data(*timestamps, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, *results, kRaw * 8);
    dev.set_timestamp_frequency(kFrequency);
    list->resolve_query_data(*timestamps, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, *results, kScaled * 8);
    if (!run(*queue, *list, *fence, 1))
        return false;

    // A timestamp in a later submission, after the CPU has slept.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_HR(alloc->reset());
    CHECK_HR(list->reset(*alloc));
    list->end_query(*timestamps, D3D12_QUERY_TYPE_TIMESTAMP, 2);
    list->resolve_query_data(*timestamps, D3D12_QUERY_TYPE_TIMESTAMP, 0, 3, *results, kLater * 8);
    if (!run(*queue, *list, *fence, 2))
        return false;
    dev.set_timestamp_frequency(0);

    const auto* r = static_cast<const uint64_t*>(results->cpu_pointer());
    bool ok = true;
    auto expect = [&](const char* what, uint64_t got, uint64_t lo, uint64_t hi) {
        if (got < lo || got > hi) {
            std::fprintf(stderr, "  %s: %llu, expected %llu..%llu\n", what, static_cast<unsigned long long>(got),
                         static_cast<unsigned long long>(lo), static_cast<unsigned long long>(hi));
            ok = false;
        }
    };
    expect("occlusion, passing", r[kOcclusion + 0], 128, 128);
    expect("binary occlusion, passing", r[kOcclusion + 1], 1, 1);
    expect("binary occlusion, failing", r[kOcclusion + 2], 0, 0);
    expect("occlusion, failing", r[kOcclusion + 3], 0, 0);
    expect("occlusion, never used", r[kOcclusion + 4], 0, 0);

    const auto& s = *reinterpret_cast<const D3D12_QUERY_DATA_PIPELINE_STATISTICS*>(r + kStatistics);
    expect("IAVertices", s.IAVertices, 3, 3);
    expect("IAPrimitives", s.IAPrimitives, 1, 1);
    expect("VSInvocations", s.VSInvocations, 1, 3);
    expect("GSInvocations", s.GSInvocations, 0, 0);
    expect("CInvocations", s.CInvocations, 1, 1);
    expect("PSInvocations", s.PSInvocations, 1, 4 * 128);   // implementation-defined: AMD counts quads
    expect("HSInvocations", s.HSInvocations, 0, 0);
    expect("DSInvocations", s.DSInvocations, 0, 0);
    expect("CSInvocations", s.CSInvocations, 0, 0);

    // Rescaled: ticks * period * kFrequency / 1 s, give or take rounding.
    const long double scale = static_cast<long double>(kFrequency) / dev.timestamp_frequency();
    for (uint32_t i = 0; i < 2; i++) {
        const long double want = r[kRaw + i] * scale;
        const auto want_lo = static_cast<uint64_t>(std::floor(want * (1 - 1e-6L))),
                   want_hi = static_cast<uint64_t>(std::ceil(want * (1 + 1e-6L))) + 1;
        expect("rescaled timestamp", r[kScaled + i], want_lo, want_hi);
        expect("resolved again", r[kLater + i], r[kScaled + i], r[kScaled + i]);
    }
    expect("timestamps in order", r[kRaw + 1], r[kRaw + 0], UINT64_MAX);
    // 50 ms of sleep at kFrequency, and less than 5 s.
    expect("timestamp after the sleep", r[kLater + 2] - r[kLater + 1], kFrequency / 20 * 9 / 10, kFrequency * 5);
    std::printf("  GPU timestamps at %llu Hz; the sleep took %.1f ms\n",
                static_cast<unsigned long long>(dev.timestamp_frequency()),
                (r[kLater + 2] - r[kLater + 1]) * 1e3 / kFrequency);
    return ok;
}

} // namespace

int main() { return vgd_test::run_on_adapters("queries", queries); }
