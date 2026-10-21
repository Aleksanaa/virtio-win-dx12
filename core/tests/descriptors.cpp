// SPDX-License-Identifier: MIT
// Plan Phase 3 milestone 3, step 2: descriptor heaps and tables. A pixel shader samples a texture through a table
// in a shader-visible heap (SRV copied there from a CPU heap), with a heap sampler and a static sampler, and adds a
// table CBV, a structured buffer from the table, and a root CBV. Exact pixels.
#include "test_util.h"

#include <vgd/pipeline.h>

#include <textured_dxil.h>

#include <cfloat>
#include <cstdint>

namespace {

constexpr uint32_t kSize = 64, kTex = 4;
constexpr uint32_t kRowPitch = kSize * 4;

uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    return r | g << 8 | b << 16 | a << 24;
}

D3D12_SAMPLER_DESC point_clamp()
{
    D3D12_SAMPLER_DESC s{};
    s.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    s.MaxLOD = D3D12_FLOAT32_MAX;
    return s;
}

D3D12_RESOURCE_DESC texture_desc(uint32_t size, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = d.Height = size;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc = {1, 0};
    d.Flags = flags;
    return d;
}

bool descriptors(vgd::Device& dev)
{
    if (!dev.adapter().bindless) {
        std::fprintf(stderr, "  no mutable descriptors on this adapter\n");
        return false;
    }

    // Root signature: table {b0, t0-t1} and table {s0} for the pixel shader, root CBV b1, static sampler s1.
    D3D12_DESCRIPTOR_RANGE1 resource_ranges[2] = {
        {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE,
         D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND},
    };
    D3D12_DESCRIPTOR_RANGE1 sampler_range{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0,
                                          D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0};
    D3D12_ROOT_PARAMETER1 params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = {2, resource_ranges};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {1, &sampler_range};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[2].Descriptor = {1, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE};
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    const D3D12_SAMPLER_DESC ps = point_clamp();
    D3D12_STATIC_SAMPLER_DESC static_sampler{ps.Filter, ps.AddressU, ps.AddressV, ps.AddressW, 0, 1,
                                             D3D12_COMPARISON_FUNC_NEVER, D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK,
                                             0, D3D12_FLOAT32_MAX, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL};
    D3D12_ROOT_SIGNATURE_DESC1 rs_desc{3, params, 1, &static_sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    std::unique_ptr<vgd::RootSignature> rs_unique;
    CHECK_HR(vgd::RootSignature::create(rs_desc, &rs_unique));
    std::shared_ptr<const vgd::RootSignature> rs = std::move(rs_unique);

    std::shared_ptr<const vgd::Shader> vs, ps_shader;
    CHECK_HR(vgd::Shader::from_code(vgd::ShaderStage::vertex, k_vs_fullscreen, &vs));
    CHECK_HR(vgd::Shader::from_code(vgd::ShaderStage::pixel, k_ps_textured, &ps_shader));
    vgd::GraphicsPipelineDesc g;
    g.root_signature = rs;
    g.vs = vs;
    g.ps = ps_shader;
    g.blend.RenderTarget[0].SrcBlend = g.blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    g.blend.RenderTarget[0].DestBlend = g.blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    g.blend.RenderTarget[0].BlendOp = g.blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    g.blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    g.rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    g.rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    g.rasterizer.DepthClipEnable = TRUE;
    g.topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    g.num_render_targets = 1;
    g.rtv_formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    std::unique_ptr<vgd::PipelineState> pso;
    CHECK_HR(vgd::PipelineState::create_graphics(g, &pso));
    CHECK_HR(pso->compile(dev));

    // Resources: the 4x4 texture and its upload, constants (scale at 0, bias at 256), the structured buffer.
    std::unique_ptr<vgd::Resource> tex, upload, constants, offsets, target, readback;
    CHECK_HR(dev.create_committed_resource(D3D12_HEAP_TYPE_DEFAULT, texture_desc(kTex, D3D12_RESOURCE_FLAG_NONE), &tex));
    CHECK_HR(dev.create_committed_buffer(D3D12_HEAP_TYPE_UPLOAD, 256 * kTex, D3D12_RESOURCE_FLAG_NONE, &upload));
    for (uint32_t y = 0; y < kTex; y++)
        for (uint32_t x = 0; x < kTex; x++)
            static_cast<uint32_t*>(upload->cpu_pointer())[y * 64 + x] = rgba(x * 64, y * 64, 128, 255);
    CHECK_HR(dev.create_committed_buffer(D3D12_HEAP_TYPE_UPLOAD, 512, D3D12_RESOURCE_FLAG_NONE, &constants));
    const float scale[4] = {1, 1, 1, 0}, bias[4] = {0, 0, 0, 0.4f};
    std::memcpy(constants->cpu_pointer(), scale, sizeof(scale));
    std::memcpy(static_cast<uint8_t*>(constants->cpu_pointer()) + 256, bias, sizeof(bias));
    CHECK_HR(dev.create_committed_buffer(D3D12_HEAP_TYPE_UPLOAD, 32, D3D12_RESOURCE_FLAG_NONE, &offsets));
    const float offset_data[8] = {0.5f, 0.5f, 0.5f, 0.5f, 0, 0, 0, 0.2f};   // [0] would show a wrong index
    std::memcpy(offsets->cpu_pointer(), offset_data, sizeof(offset_data));
    CHECK_HR(dev.create_committed_resource(
        D3D12_HEAP_TYPE_DEFAULT, texture_desc(kSize, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET), &target));
    CHECK_HR(dev.create_committed_buffer(D3D12_HEAP_TYPE_READBACK, kRowPitch * kSize, D3D12_RESOURCE_FLAG_NONE,
                                         &readback));

    // Heaps: the texture SRV is made in a CPU heap and copied into the shader-visible one; the rest is written there
    // directly. The table starts at slot 5, the sampler at slot 2, so table offsets matter.
    std::unique_ptr<vgd::DescriptorHeap> cpu_heap, gpu_heap, sampler_heap;
    CHECK_HR(dev.create_descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, false, &cpu_heap));
    CHECK_HR(dev.create_descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16, true, &gpu_heap));
    CHECK_HR(dev.create_descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 4, true, &sampler_heap));
    CHECK(gpu_heap->shader_visible() && !cpu_heap->shader_visible() && gpu_heap->gpu_start());

    vgd::Descriptor d;
    CHECK_HR(dev.create_shader_resource_view(tex.get(), nullptr, &d));
    dev.write_descriptor(cpu_heap->cpu_start() + 3, d);
    dev.copy_descriptors(gpu_heap->cpu_start() + 6, cpu_heap->cpu_start() + 3, 1);
    const D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{constants->gpu_virtual_address(), 256};
    CHECK_HR(dev.create_constant_buffer_view(cbv, &d));
    dev.write_descriptor(gpu_heap->cpu_start() + 5, d);
    D3D12_SHADER_RESOURCE_VIEW_DESC sb{};
    sb.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sb.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sb.Buffer = {0, 2, 16, D3D12_BUFFER_SRV_FLAG_NONE};
    CHECK_HR(dev.create_shader_resource_view(offsets.get(), &sb, &d));
    dev.write_descriptor(gpu_heap->cpu_start() + 7, d);
    CHECK_HR(dev.create_sampler(ps, &d));
    dev.write_descriptor(sampler_heap->cpu_start() + 2, d);

    vgd::RenderTargetView rtv;
    CHECK_HR(dev.create_render_target_view(*target, nullptr, &rtv));

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

    // Upload the texture.
    vgd::TextureCopyLocation dst, src;
    dst.resource = tex.get();
    src.resource = upload.get();
    src.is_footprint = true;
    src.footprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kTex, kTex, 1, 256};
    list->copy_texture_region(dst, 0, 0, 0, src, nullptr);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    list->resource_barrier({&barrier, 1});

    const D3D12_VIEWPORT viewport{0, 0, kSize, kSize, 0, 1};
    const D3D12_RECT scissor{0, 0, kSize, kSize};
    vgd::DescriptorHeap* heaps[] = {gpu_heap.get(), sampler_heap.get()};
    list->set_descriptor_heaps(heaps);
    list->set_render_targets({&rtv, 1});
    list->set_viewports({&viewport, 1});
    list->set_scissor_rects({&scissor, 1});
    list->set_primitive_topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->set_graphics_root_signature(rs.get());
    list->set_pipeline_state(pso.get());
    list->set_graphics_root_descriptor_table(0, gpu_heap->gpu_start() + 5 * vgd::kDescriptorStride);
    list->set_graphics_root_descriptor_table(1, sampler_heap->gpu_start() + 2 * vgd::kDescriptorStride);
    list->set_graphics_root_descriptor(2, constants->gpu_virtual_address() + 256);
    list->draw_instanced(3, 1, 0, 0);

    list->resource_barrier({&barrier, 1});
    vgd::TextureCopyLocation rb, rt;
    rb.resource = readback.get();
    rb.is_footprint = true;
    rb.footprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, 1, kRowPitch};
    rt.resource = target.get();
    list->copy_texture_region(rb, 0, 0, 0, rt, nullptr);
    CHECK_HR(list->close());

    vgd::CommandList* cl = list.get();
    CHECK_HR(queue->execute_command_lists({&cl, 1}, fence.get(), 1));
    CHECK(fence->wait(1, 5'000'000'000ull) == S_OK);

    // Pixel (x, y) shows texel (x / 16, y / 16); alpha is 1 * 0 + 0.2 + 0.4.
    const auto* px = static_cast<const uint32_t*>(readback->cpu_pointer());
    bool ok = true;
    for (uint32_t y : {8u, 24u, 40u, 56u})
        for (uint32_t x : {8u, 24u, 40u, 56u}) {
            const uint32_t expected = rgba(x / 16 * 64, y / 16 * 64, 128, 153), got = px[y * kSize + x];
            if (got != expected) {
                std::fprintf(stderr, "  pixel (%u, %u), %s sampler: 0x%08x, expected 0x%08x\n", x, y,
                             x < 32 ? "heap" : "static", got, expected);
                ok = false;
            }
        }
    return ok;
}

} // namespace

int main() { return vgd_test::run_on_adapters("descriptors", descriptors); }
