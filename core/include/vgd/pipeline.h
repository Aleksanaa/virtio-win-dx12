// SPDX-License-Identifier: MIT
#pragma once

#include <vgd/d3d12.h>
#include <vgd/root_signature.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace vgd {

enum class ShaderStage : uint8_t { vertex, hull, domain, geometry, pixel, compute, amplification, mesh };
const char* stage_name(ShaderStage stage);

// Shader bytecode: DXIL (or legacy DXBC), either as a DXBC-format container or as the bare program the DDI passes.
class Shader {
public:
    enum class Format : uint8_t { container, dxil_program, dxbc_program };

    // The size comes from the code itself (container header or program header).
    static HRESULT from_code(ShaderStage stage, const void* code, std::shared_ptr<const Shader>* out);

    ShaderStage stage() const { return stage_; }
    std::span<const uint8_t> code() const { return code_; }
    bool is_dxil() const { return dxil_; }
    Format format() const { return format_; }

private:
    Shader() = default;
    ShaderStage stage_{};
    std::vector<uint8_t> code_;
    bool dxil_ = false;
    Format format_ = Format::container;
};

// One input-assembler element. The DDI names inputs by shader input register, not semantic.
struct VertexElement {
    uint32_t input_register;
    uint32_t slot;
    uint32_t offset;
    DXGI_FORMAT format;
    bool per_instance;
    uint32_t step_rate;
};

struct GraphicsPipelineDesc {
    std::shared_ptr<const RootSignature> root_signature;
    std::shared_ptr<const Shader> vs, hs, ds, gs, ps;
    D3D12_BLEND_DESC blend{};
    uint32_t sample_mask = UINT32_MAX;
    D3D12_RASTERIZER_DESC rasterizer{};
    D3D12_DEPTH_STENCIL_DESC1 depth_stencil{};
    std::vector<VertexElement> input_layout;
    D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
    uint32_t num_render_targets = 0;
    std::array<DXGI_FORMAT, 8> rtv_formats{};
    DXGI_FORMAT dsv_format = DXGI_FORMAT_UNKNOWN;
    DXGI_SAMPLE_DESC sample_desc{1, 0};
};

struct ComputePipelineDesc {
    std::shared_ptr<const RootSignature> root_signature;
    std::shared_ptr<const Shader> cs;
};

// A pipeline state object. It keeps everything it was created from; the Vulkan pipeline is compiled when the
// pipeline is first used (shader translation and descriptor heap mapping come with the graphics milestone).
class PipelineState {
public:
    static HRESULT create_graphics(GraphicsPipelineDesc desc, std::unique_ptr<PipelineState>* out);
    static HRESULT create_compute(ComputePipelineDesc desc, std::unique_ptr<PipelineState>* out);

    bool is_compute() const { return compute_; }
    const GraphicsPipelineDesc& graphics() const { return graphics_; }
    const ComputePipelineDesc& compute() const { return compute_desc_; }

private:
    PipelineState() = default;
    bool compute_ = false;
    GraphicsPipelineDesc graphics_;
    ComputePipelineDesc compute_desc_;
};

} // namespace vgd
