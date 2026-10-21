// SPDX-License-Identifier: MIT
// Plan Phase 3 milestone 3, first step: an off-screen draw with exact pixels. DXIL shaders (dxc), root constants,
// a vertex buffer, viewport/scissor, culling, render target clear, and a texture -> buffer copy for readback.
#include "test_util.h"

#include <vgd/pipeline.h>

#include <triangle_dxil.h>

#include <cstdint>

namespace {

constexpr uint32_t kSize = 64;
constexpr uint32_t kRowPitch = kSize * 4;   // a multiple of D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256)

D3D12_BLEND_DESC default_blend()
{
    D3D12_BLEND_DESC b{};
    for (auto& rt : b.RenderTarget)
        rt = {FALSE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_ZERO,
              D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL};
    return b;
}

D3D12_RASTERIZER_DESC default_rasterizer()
{
    D3D12_RASTERIZER_DESC r{};
    r.FillMode = D3D12_FILL_MODE_SOLID;
    r.CullMode = D3D12_CULL_MODE_BACK;   // clockwise is front: a winding mix-up culls the triangles
    r.DepthClipEnable = TRUE;
    return r;
}

uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return r | g << 8 | b << 16 | uint32_t(a) << 24;
}

bool triangle(vgd::Device& dev)
{
    // Root signature: four 32-bit constants at b0 for the pixel shader.
    D3D12_ROOT_PARAMETER1 param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants = {0, 0, 4};
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC1 rs_desc{1, &param, 0, nullptr,
                                       D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    std::unique_ptr<vgd::RootSignature> rs_unique;
    CHECK_HR(vgd::RootSignature::create(rs_desc, &rs_unique));
    std::shared_ptr<const vgd::RootSignature> rs = std::move(rs_unique);

    std::shared_ptr<const vgd::Shader> vs_corner, vs_buffer, ps;
    CHECK_HR(vgd::Shader::from_code(vgd::ShaderStage::vertex, k_vs_corner, &vs_corner));
    CHECK_HR(vgd::Shader::from_code(vgd::ShaderStage::vertex, k_vs_buffer, &vs_buffer));
    CHECK_HR(vgd::Shader::from_code(vgd::ShaderStage::pixel, k_ps_color, &ps));

    vgd::GraphicsPipelineDesc g;
    g.root_signature = rs;
    g.vs = vs_corner;
    g.ps = ps;
    g.blend = default_blend();
    g.rasterizer = default_rasterizer();
    g.topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    g.num_render_targets = 1;
    g.rtv_formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    std::unique_ptr<vgd::PipelineState> pso_corner, pso_buffer;
    CHECK_HR(vgd::PipelineState::create_graphics(g, &pso_corner));
    CHECK_HR(pso_corner->compile(dev));
    g.vs = vs_buffer;
    g.input_layout = {{0, 0, 0, DXGI_FORMAT_R32G32_FLOAT, false, 0}};
    CHECK_HR(vgd::PipelineState::create_graphics(g, &pso_buffer));
    CHECK_HR(pso_buffer->compile(dev));

    // The bottom-right corner, through a vertex buffer: (1, 0), (1, -1), (0, -1), clockwise on screen.
    const float vertices[] = {1, 0, 1, -1, 0, -1};
    std::unique_ptr<vgd::Resource> vb, readback, target;
    CHECK_HR(dev.create_committed_buffer(D3D12_HEAP_TYPE_UPLOAD, sizeof(vertices), D3D12_RESOURCE_FLAG_NONE, &vb));
    std::memcpy(vb->cpu_pointer(), vertices, sizeof(vertices));
    CHECK_HR(dev.create_committed_buffer(D3D12_HEAP_TYPE_READBACK, kRowPitch * kSize, D3D12_RESOURCE_FLAG_NONE,
                                         &readback));

    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = td.Height = kSize;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc = {1, 0};
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    uint64_t size = 0, alignment = 0;
    CHECK_HR(dev.resource_allocation_info(td, &size, &alignment));
    CHECK(size >= kSize * kSize * 4 && alignment >= 65536 && size % alignment == 0);
    CHECK_HR(dev.create_committed_resource(D3D12_HEAP_TYPE_DEFAULT, td, &target));
    CHECK(target->is_texture() && target->gpu_virtual_address() == 0);
    vgd::RenderTargetView rtv;
    CHECK_HR(dev.create_render_target_view(*target, nullptr, &rtv));
    CHECK(rtv.width == kSize && rtv.height == kSize);

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

    const float clear[4] = {0.0f, 0.0f, 0.6f, 1.0f};
    const float color_a[4] = {1.0f, 0.2f, 0.0f, 1.0f};
    const float color_b[4] = {0.0f, 0.4f, 1.0f, 1.0f};
    const D3D12_VIEWPORT viewport{0, 0, kSize, kSize, 0, 1};
    const D3D12_RECT scissor{0, 0, kSize, kSize};
    const D3D12_VERTEX_BUFFER_VIEW vbv{vb->gpu_virtual_address(), sizeof(vertices), 8};

    list->clear_render_target_view(rtv, clear, {});
    list->set_render_targets({&rtv, 1});
    list->set_viewports({&viewport, 1});
    list->set_scissor_rects({&scissor, 1});
    list->set_primitive_topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->set_graphics_root_signature(rs.get());
    list->set_pipeline_state(pso_corner.get());
    list->set_graphics_root_32bit_constants(0, {reinterpret_cast<const uint32_t*>(color_a), 4}, 0);
    list->draw_instanced(3, 1, 0, 0);
    list->set_pipeline_state(pso_buffer.get());
    list->set_vertex_buffers(0, {&vbv, 1});
    list->set_graphics_root_32bit_constants(0, {reinterpret_cast<const uint32_t*>(color_b), 4}, 0);
    list->draw_instanced(3, 1, 0, 0);
    // A clear with a rect, inside rendering-free territory: the top-right 8x8 corner.
    const D3D12_RECT corner{kSize - 8, 0, kSize, 8};
    list->clear_render_target_view(rtv, color_b, {&corner, 1});

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = nullptr;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->resource_barrier({&barrier, 1});
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

    const auto* px = static_cast<const uint32_t*>(readback->cpu_pointer());
    auto at = [&](uint32_t x, uint32_t y) { return px[y * kSize + x]; };
    const uint32_t a = rgba(255, 51, 0, 255), b = rgba(0, 102, 255, 255), c = rgba(0, 0, 153, 255);
    struct Probe { uint32_t x, y, expected; const char* what; } probes[] = {
        {4, 4, a, "top-left corner (vertex id triangle)"},
        {60, 60, b, "bottom-right corner (vertex buffer triangle)"},
        {4, 60, c, "bottom-left (clear)"},
        {32, 32, c, "center (clear)"},
        {60, 2, b, "top-right (rect clear)"},
        {52, 2, c, "next to the rect clear"},
    };
    bool ok = true;
    for (const Probe& p : probes)
        if (at(p.x, p.y) != p.expected) {
            std::fprintf(stderr, "  pixel (%u, %u), %s: 0x%08x, expected 0x%08x\n", p.x, p.y, p.what, at(p.x, p.y),
                         p.expected);
            ok = false;
        }
    return ok;
}

} // namespace

int main() { return vgd_test::run_on_adapters("triangle", triangle); }
