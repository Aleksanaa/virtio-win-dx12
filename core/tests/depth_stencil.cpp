// SPDX-License-Identifier: MIT
// Plan Phase 3 milestone 3, step 3: depth/stencil and MSAA. Full and rect clears of each plane, depth test and
// write, a stencil-equal pass, then a 4x MSAA draw resolved to a single-sampled target. Exact pixels.
#include "test_util.h"

#include <vgd/pipeline.h>

#include <depth_dxil.h>

#include <cstdint>

namespace {

constexpr uint32_t kSize = 64, kRowPitch = kSize * 4;

uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    return r | g << 8 | b << 16 | a << 24;
}

D3D12_RESOURCE_DESC texture_desc(DXGI_FORMAT format, uint32_t samples, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = d.Height = kSize;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc = {samples, 0};
    d.Flags = flags;
    return d;
}

// Runs one closed list and waits for it.
bool run(vgd::Device& dev, vgd::CommandQueue& queue, vgd::CommandList& list, vgd::Fence& fence, uint64_t value)
{
    CHECK_HR(list.close());
    vgd::CommandList* cl = &list;
    CHECK_HR(queue.execute_command_lists({&cl, 1}, &fence, value));
    CHECK(fence.wait(value, 5'000'000'000ull) == S_OK);
    return true;
}

void copy_to_readback(vgd::CommandList& list, vgd::Resource& texture, vgd::Resource& readback)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    list.resource_barrier({&barrier, 1});
    vgd::TextureCopyLocation dst, src;
    dst.resource = &readback;
    dst.is_footprint = true;
    dst.footprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, 1, kRowPitch};
    src.resource = &texture;
    list.copy_texture_region(dst, 0, 0, 0, src, nullptr);
}

struct Probe {
    uint32_t x, y, expected;
    const char* what;
};

bool check(const vgd::Resource& readback, std::initializer_list<Probe> probes)
{
    const auto* px = static_cast<const uint32_t*>(const_cast<vgd::Resource&>(readback).cpu_pointer());
    bool ok = true;
    for (const Probe& p : probes)
        if (px[p.y * kSize + p.x] != p.expected) {
            std::fprintf(stderr, "  pixel (%u, %u), %s: 0x%08x, expected 0x%08x\n", p.x, p.y, p.what,
                         px[p.y * kSize + p.x], p.expected);
            ok = false;
        }
    return ok;
}

bool depth_stencil(vgd::Device& dev)
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

    const DXGI_FORMAT ds_format = DXGI_FORMAT_D24_UNORM_S8_UINT;   // D32S8 on GPUs without D24S8
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
    g.dsv_format = ds_format;
    g.depth_stencil.DepthEnable = TRUE;
    g.depth_stencil.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    g.depth_stencil.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    std::unique_ptr<vgd::PipelineState> pso_depth, pso_stencil, pso_msaa;
    CHECK_HR(vgd::PipelineState::create_graphics(g, &pso_depth));
    CHECK_HR(pso_depth->compile(dev));
    g.depth_stencil.StencilEnable = TRUE;
    g.depth_stencil.StencilReadMask = g.depth_stencil.StencilWriteMask = 0xff;
    g.depth_stencil.FrontFace = g.depth_stencil.BackFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                                            D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_EQUAL};
    CHECK_HR(vgd::PipelineState::create_graphics(g, &pso_stencil));
    CHECK_HR(pso_stencil->compile(dev));
    g.depth_stencil = {};
    g.dsv_format = DXGI_FORMAT_UNKNOWN;
    g.sample_desc = {4, 0};
    CHECK_HR(vgd::PipelineState::create_graphics(g, &pso_msaa));
    CHECK_HR(pso_msaa->compile(dev));

    std::unique_ptr<vgd::Resource> target, depth, msaa, resolved, readback;
    CHECK_HR(dev.create_committed_resource(
        D3D12_HEAP_TYPE_DEFAULT, texture_desc(DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
        &target));
    CHECK_HR(dev.create_committed_resource(
        D3D12_HEAP_TYPE_DEFAULT, texture_desc(ds_format, 1, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL), &depth));
    CHECK_HR(dev.create_committed_resource(
        D3D12_HEAP_TYPE_DEFAULT, texture_desc(DXGI_FORMAT_R8G8B8A8_UNORM, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
        &msaa));
    CHECK_HR(dev.create_committed_resource(
        D3D12_HEAP_TYPE_DEFAULT, texture_desc(DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D12_RESOURCE_FLAG_NONE), &resolved));
    CHECK_HR(dev.create_committed_buffer(D3D12_HEAP_TYPE_READBACK, kRowPitch * kSize, D3D12_RESOURCE_FLAG_NONE,
                                         &readback));
    vgd::RenderTargetView rtv, msaa_rtv;
    vgd::DepthStencilView dsv;
    CHECK_HR(dev.create_render_target_view(*target, nullptr, &rtv));
    CHECK_HR(dev.create_render_target_view(*msaa, nullptr, &msaa_rtv));
    CHECK_HR(dev.create_depth_stencil_view(*depth, nullptr, &dsv));
    CHECK(dsv.aspects == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));

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

    const float clear[4] = {0, 0, 0.6f, 1}, a[4] = {1, 0.2f, 0, 1}, b[4] = {0, 0.4f, 1, 1}, c[4] = {0.2f, 1, 0.2f, 1};
    const uint32_t clear_px = rgba(0, 0, 153, 255), a_px = rgba(255, 51, 0, 255), b_px = rgba(0, 102, 255, 255),
                   c_px = rgba(51, 255, 51, 255);
    const D3D12_VIEWPORT viewport{0, 0, kSize, kSize, 0, 1};
    const D3D12_RECT scissor{0, 0, kSize, kSize};
    const D3D12_RECT top_right{kSize - 16, 0, kSize, 16}, bottom_left{0, kSize - 16, 16, kSize};
    auto draw = [&](vgd::PipelineState& pso, float z, const float* color) {
        list->set_pipeline_state(&pso);
        list->set_graphics_root_32bit_constants(0, {reinterpret_cast<const uint32_t*>(&z), 1}, 0);
        list->set_graphics_root_32bit_constants(1, {reinterpret_cast<const uint32_t*>(color), 4}, 0);
        list->draw_instanced(3, 1, 0, 0);
    };

    // Depth 1 and stencil 0 everywhere; depth 0 in the top-right corner, stencil 1 in the bottom-left.
    list->clear_render_target_view(rtv, clear, {});
    list->clear_depth_stencil_view(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, {});
    list->clear_depth_stencil_view(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, {&top_right, 1});
    list->clear_depth_stencil_view(dsv, D3D12_CLEAR_FLAG_STENCIL, 0.0f, 1, {&bottom_left, 1});
    list->set_render_targets({&rtv, 1}, &dsv);
    list->set_viewports({&viewport, 1});
    list->set_scissor_rects({&scissor, 1});
    list->set_primitive_topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->set_graphics_root_signature(rs.get());
    list->set_stencil_ref(1);
    draw(*pso_depth, 0.5f, a);     // everywhere but the depth-0 corner
    draw(*pso_depth, 0.7f, b);     // behind: nowhere
    draw(*pso_stencil, 0.2f, c);   // in front, but only where stencil is 1
    copy_to_readback(*list, *target, *readback);
    if (!run(dev, *queue, *list, *fence, 1))
        return false;
    bool ok = check(*readback, {
        {4, 4, a_px, "depth test passed"},
        {32, 32, a_px, "center"},
        {60, 4, clear_px, "depth rect clear (0) rejects"},
        {60, 20, a_px, "below the depth rect"},
        {4, 60, c_px, "stencil rect"},
        {20, 60, a_px, "next to the stencil rect"},
        {60, 60, a_px, "bottom right"},
    });

    // 4x MSAA, resolved.
    CHECK_HR(alloc->reset());
    CHECK_HR(list->reset(*alloc));
    list->clear_render_target_view(msaa_rtv, clear, {});
    list->set_render_targets({&msaa_rtv, 1});
    list->set_viewports({&viewport, 1});
    list->set_scissor_rects({&scissor, 1});
    list->set_primitive_topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->set_graphics_root_signature(rs.get());
    draw(*pso_msaa, 0.5f, b);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    list->resource_barrier({&barrier, 1});
    list->resolve_subresource(*resolved, 0, *msaa, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    copy_to_readback(*list, *resolved, *readback);
    if (!run(dev, *queue, *list, *fence, 2))
        return false;
    ok &= check(*readback, {{4, 4, b_px, "MSAA resolve"}, {60, 60, b_px, "MSAA resolve"}});
    return ok;
}

} // namespace

int main() { return vgd_test::run_on_adapters("depth_stencil", depth_stencil); }
