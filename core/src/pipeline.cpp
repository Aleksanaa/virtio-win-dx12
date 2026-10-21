// SPDX-License-Identifier: MIT
#include <vgd/pipeline.h>

#include "internal.h"

#include <cstring>

namespace vgd {

const char* stage_name(ShaderStage stage)
{
    static const char* names[] = {"vertex", "hull", "domain", "geometry", "pixel", "compute", "amplification", "mesh"};
    return names[static_cast<int>(stage)];
}

HRESULT Shader::from_code(ShaderStage stage, const void* code, std::shared_ptr<const Shader>* out)
{
    if (!code || !out)
        return E_INVALIDARG;
    const auto* bytes = static_cast<const uint8_t*>(code);
    std::shared_ptr<Shader> s(new Shader);
    s->stage_ = stage;
    uint32_t size = 0;

    uint32_t program = 0;   // offset of the DXIL program (header + bitcode) in the code
    if (std::memcmp(bytes, "DXBC", 4) == 0) {
        // Container: "DXBC", 16-byte digest, u16 major, u16 minor, u32 total size, u32 part count, part offsets;
        // each part starts with a FourCC and a u32 size.
        uint32_t parts;
        std::memcpy(&size, bytes + 24, 4);
        std::memcpy(&parts, bytes + 28, 4);
        if (size < 32 + 4 * parts || size > (64u << 20))
            return E_INVALIDARG;
        s->format_ = Format::container;
        for (uint32_t i = 0; i < parts; i++) {
            uint32_t offset;
            std::memcpy(&offset, bytes + 32 + 4 * i, 4);
            if (offset + 8 <= size && std::memcmp(bytes + offset, "DXIL", 4) == 0) {
                s->dxil_ = true;
                program = offset + 8;
            }
        }
    } else {
        // A bare program, as the DDI passes it. Both kinds keep their size in 32-bit words in the second word:
        // DXIL: {u32 program version, u32 size in words, "DXIL", u32 DXIL version, u32 bitcode offset, u32 size}
        // DXBC: {u32 version token, u32 length in words, tokens...}
        uint32_t words;
        std::memcpy(&words, bytes + 4, 4);
        if (words < 2 || words > (16u << 20))
            return E_INVALIDARG;
        size = 4 * words;
        s->dxil_ = size >= 24 && std::memcmp(bytes + 8, "DXIL", 4) == 0;
        s->format_ = s->dxil_ ? Format::dxil_program : Format::dxbc_program;
    }
    s->code_.assign(bytes, bytes + size);

    // Program header: {u32 program version, u32 size in words, "DXIL", u32 DXIL version, u32 bitcode offset (from
    // the "DXIL" magic), u32 bitcode size}.
    if (s->dxil_ && program + 24 <= size) {
        uint32_t bc_offset, bc_size;
        std::memcpy(&bc_offset, bytes + program + 16, 4);
        std::memcpy(&bc_size, bytes + program + 20, 4);
        const uint64_t start = uint64_t(program) + 8 + bc_offset;
        if (start + bc_size <= size) {
            s->bitcode_offset_ = start;
            s->bitcode_size_ = bc_size;
        }
    }
    *out = std::move(s);
    return S_OK;
}

HRESULT PipelineState::create_graphics(GraphicsPipelineDesc desc, std::unique_ptr<PipelineState>* out)
{
    if (!out || !desc.root_signature)
        return E_INVALIDARG;
    if (!desc.vs) {
        log(LogLevel::warn, "graphics pipeline without a vertex shader (mesh pipelines are not supported)");
        return E_INVALIDARG;
    }
    if (desc.num_render_targets > 8)
        return E_INVALIDARG;
    std::unique_ptr<PipelineState> p(new PipelineState);
    p->graphics_ = std::move(desc);
    *out = std::move(p);
    return S_OK;
}

HRESULT PipelineState::create_compute(ComputePipelineDesc desc, std::unique_ptr<PipelineState>* out)
{
    if (!out || !desc.root_signature || !desc.cs)
        return E_INVALIDARG;
    std::unique_ptr<PipelineState> p(new PipelineState);
    p->compute_ = true;
    p->compute_desc_ = std::move(desc);
    *out = std::move(p);
    return S_OK;
}

} // namespace vgd

namespace vgd {

PipelineState::PipelineState() = default;
PipelineState::~PipelineState() = default;

PipelineState::Impl::~Impl()
{
    if (pipeline)
        device->vk.vkDestroyPipeline(device->device, pipeline, nullptr);
    if (layout)
        device->vk.vkDestroyPipelineLayout(device->device, layout, nullptr);
}

namespace {

// D3D12 enums that are Vulkan's plus one.
template <class V, class D>
V minus_one(D v, V fallback)
{
    return static_cast<int>(v) > 0 ? static_cast<V>(static_cast<int>(v) - 1) : fallback;
}

VkBlendFactor blend_factor(D3D12_BLEND b)
{
    switch (b) {
    case D3D12_BLEND_ZERO: return VK_BLEND_FACTOR_ZERO;
    case D3D12_BLEND_ONE: return VK_BLEND_FACTOR_ONE;
    case D3D12_BLEND_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
    case D3D12_BLEND_INV_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case D3D12_BLEND_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
    case D3D12_BLEND_INV_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case D3D12_BLEND_DEST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
    case D3D12_BLEND_INV_DEST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case D3D12_BLEND_DEST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
    case D3D12_BLEND_INV_DEST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case D3D12_BLEND_SRC_ALPHA_SAT: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case D3D12_BLEND_BLEND_FACTOR: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case D3D12_BLEND_INV_BLEND_FACTOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case D3D12_BLEND_SRC1_COLOR: return VK_BLEND_FACTOR_SRC1_COLOR;
    case D3D12_BLEND_INV_SRC1_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case D3D12_BLEND_SRC1_ALPHA: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case D3D12_BLEND_INV_SRC1_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case D3D12_BLEND_ALPHA_FACTOR: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case D3D12_BLEND_INV_ALPHA_FACTOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

VkLogicOp logic_op(D3D12_LOGIC_OP op)
{
    static constexpr VkLogicOp ops[] = {
        VK_LOGIC_OP_CLEAR, VK_LOGIC_OP_SET, VK_LOGIC_OP_COPY, VK_LOGIC_OP_COPY_INVERTED,
        VK_LOGIC_OP_NO_OP, VK_LOGIC_OP_INVERT, VK_LOGIC_OP_AND, VK_LOGIC_OP_NAND,
        VK_LOGIC_OP_OR, VK_LOGIC_OP_NOR, VK_LOGIC_OP_XOR, VK_LOGIC_OP_EQUIVALENT,
        VK_LOGIC_OP_AND_REVERSE, VK_LOGIC_OP_AND_INVERTED, VK_LOGIC_OP_OR_REVERSE, VK_LOGIC_OP_OR_INVERTED,
    };
    return static_cast<unsigned>(op) < std::size(ops) ? ops[op] : VK_LOGIC_OP_NO_OP;
}

VkStencilOpState stencil_state(const D3D12_DEPTH_STENCILOP_DESC& s, uint8_t read_mask, uint8_t write_mask)
{
    VkStencilOpState o{};
    o.failOp = minus_one(s.StencilFailOp, VK_STENCIL_OP_KEEP);
    o.passOp = minus_one(s.StencilPassOp, VK_STENCIL_OP_KEEP);
    o.depthFailOp = minus_one(s.StencilDepthFailOp, VK_STENCIL_OP_KEEP);
    o.compareOp = minus_one(s.StencilFunc, VK_COMPARE_OP_ALWAYS);
    o.compareMask = read_mask;
    o.writeMask = write_mask;
    return o;
}

VkPrimitiveTopology topology_class(D3D12_PRIMITIVE_TOPOLOGY_TYPE t)
{
    switch (t) {
    case D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

// Shader modules for a pipeline's stages, destroyed once the pipeline exists.
struct Stages {
    Device::Impl& d;
    std::vector<VkPipelineShaderStageCreateInfo> infos;
    std::vector<std::string> entries;   // stable storage for pName
    std::vector<VkShaderModule> modules;
    explicit Stages(Device::Impl& dev) : d(dev) { entries.reserve(5); }
    ~Stages()
    {
        for (VkShaderModule m : modules)
            d.vk.vkDestroyShaderModule(d.device, m, nullptr);
    }

    HRESULT add(const Shader& shader, const RootSignature& rs)
    {
        std::vector<uint32_t> spirv;
        std::string entry;
        HRESULT hr = translate_shader(shader, rs, &spirv, &entry);
        if (FAILED(hr))
            return hr;
        VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mci.codeSize = spirv.size() * 4;
        mci.pCode = spirv.data();
        VkShaderModule m;
        VkResult r = d.vk.vkCreateShaderModule(d.device, &mci, nullptr, &m);
        if (r != VK_SUCCESS)
            return hresult_from_vk(r);
        modules.push_back(m);
        entries.push_back(std::move(entry));
        VkPipelineShaderStageCreateInfo si{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        si.stage = vk_stage(shader.stage());
        si.module = m;
        si.pName = entries.back().c_str();
        infos.push_back(si);
        return S_OK;
    }
};

} // namespace

HRESULT PipelineState::compile(Device& device)
{
    Device::Impl& d = device.impl();
    const RootSignature& rs = root_signature();
    auto impl = std::make_unique<Impl>();
    impl->device = &d;
    impl->push_size = rs.root_data_size();
    if (impl->push_size > d.physical->limits.maxPushConstantsSize) {
        log(LogLevel::warn, "root data of %u bytes exceeds the device's %u bytes of push constants", impl->push_size,
            d.physical->limits.maxPushConstantsSize);
        return E_NOTIMPL;
    }
    VkPushConstantRange push{VK_SHADER_STAGE_ALL, 0, impl->push_size};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.pushConstantRangeCount = impl->push_size ? 1 : 0;
    lci.pPushConstantRanges = &push;
    VkResult r = d.vk.vkCreatePipelineLayout(d.device, &lci, nullptr, &impl->layout);
    if (r != VK_SUCCESS)
        return hresult_from_vk(r);

    Stages stages(d);
    if (compute_) {
        HRESULT hr = stages.add(*compute_desc_.cs, rs);
        if (FAILED(hr))
            return hr;
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = stages.infos[0];
        ci.layout = impl->layout;
        r = d.vk.vkCreateComputePipelines(d.device, VK_NULL_HANDLE, 1, &ci, nullptr, &impl->pipeline);
        if (r != VK_SUCCESS)
            return hresult_from_vk(r);
        impl->bind_point = VK_PIPELINE_BIND_POINT_COMPUTE;
        impl_ = std::move(impl);
        return S_OK;
    }

    const GraphicsPipelineDesc& g = graphics_;
    if (g.hs || g.ds) {
        log(LogLevel::warn, "tessellation pipelines are not supported yet");
        return E_NOTIMPL;
    }
    for (const auto* s : {g.vs.get(), g.gs.get(), g.ps.get()})
        if (s) {
            HRESULT hr = stages.add(*s, rs);
            if (FAILED(hr))
                return hr;
        }

    // Vertex input: one binding per input slot; strides come with IASetVertexBuffers (dynamic).
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    std::array<uint32_t, D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> slot_end{};
    for (const VertexElement& e : g.input_layout) {
        if (e.slot >= slot_end.size())
            return E_INVALIDARG;
        if (!(impl->vertex_binding_mask & (1u << e.slot))) {
            impl->vertex_binding_mask |= 1u << e.slot;
            bindings.push_back({e.slot, 0, e.per_instance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX});
        }
        if (e.per_instance && e.step_rate != 1)
            log(LogLevel::warn, "instance data step rate %u is not supported yet (treated as 1)", e.step_rate);
        const VkFormat f = d.resolve_format(e.format);
        const uint32_t offset = e.offset == D3D12_APPEND_ALIGNED_ELEMENT ? slot_end[e.slot] : e.offset;
        slot_end[e.slot] = offset + format_block(f).bytes;
        attributes.push_back({e.input_register, e.slot, f, offset});
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
    vi.pVertexBindingDescriptions = bindings.data();
    vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vi.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = topology_class(g.topology);
    impl->primitive_restart = g.strip_cut != D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};   // counts dynamic

    const D3D12_RASTERIZER_DESC& rd = g.rasterizer;
    VkPipelineRasterizationStateCreateInfo rs_ci{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs_ci.depthClampEnable = !rd.DepthClipEnable && d.physical->features.depthClamp;
    rs_ci.polygonMode = rd.FillMode == D3D12_FILL_MODE_WIREFRAME && d.physical->features.fillModeNonSolid
                            ? VK_POLYGON_MODE_LINE
                            : VK_POLYGON_MODE_FILL;
    rs_ci.cullMode = minus_one(rd.CullMode, VK_CULL_MODE_NONE);
    // With the negative-height viewport (y flipped to D3D's direction) the winding seen by Vulkan matches D3D's.
    rs_ci.frontFace = rd.FrontCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs_ci.depthBiasEnable = rd.DepthBias != 0 || rd.SlopeScaledDepthBias != 0.0f;
    rs_ci.depthBiasConstantFactor = static_cast<float>(rd.DepthBias);
    rs_ci.depthBiasClamp = d.physical->features.depthBiasClamp ? rd.DepthBiasClamp : 0.0f;
    rs_ci.depthBiasSlopeFactor = rd.SlopeScaledDepthBias;
    rs_ci.lineWidth = 1.0f;

    const uint32_t sample_mask[2] = {g.sample_mask, g.sample_mask};
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = static_cast<VkSampleCountFlagBits>(std::max(g.sample_desc.Count, 1u));
    ms.pSampleMask = sample_mask;
    ms.alphaToCoverageEnable = g.blend.AlphaToCoverageEnable;

    const VkFormat ds_format = g.dsv_format == DXGI_FORMAT_UNKNOWN ? VK_FORMAT_UNDEFINED : d.resolve_format(g.dsv_format);
    const VkImageAspectFlags ds_aspects = ds_format ? format_aspects(ds_format) : 0;
    const D3D12_DEPTH_STENCIL_DESC1& dsd = g.depth_stencil;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    if (ds_aspects & VK_IMAGE_ASPECT_DEPTH_BIT) {
        ds.depthTestEnable = dsd.DepthEnable;
        ds.depthWriteEnable = dsd.DepthEnable && dsd.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL;
        ds.depthCompareOp = minus_one(dsd.DepthFunc, VK_COMPARE_OP_ALWAYS);
        ds.depthBoundsTestEnable = dsd.DepthBoundsTestEnable && d.physical->features.depthBounds;
    }
    if (ds_aspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
        ds.stencilTestEnable = dsd.StencilEnable;
        ds.front = stencil_state(dsd.FrontFace, dsd.StencilReadMask, dsd.StencilWriteMask);
        ds.back = stencil_state(dsd.BackFace, dsd.StencilReadMask, dsd.StencilWriteMask);
    }
    ds.maxDepthBounds = 1.0f;

    std::array<VkPipelineColorBlendAttachmentState, 8> attachments{};
    std::array<VkFormat, 8> color_formats{};
    for (uint32_t i = 0; i < g.num_render_targets; i++) {
        const auto& t = g.blend.RenderTarget[g.blend.IndependentBlendEnable ? i : 0];
        auto& a = attachments[i];
        a.blendEnable = t.BlendEnable;
        a.srcColorBlendFactor = blend_factor(t.SrcBlend);
        a.dstColorBlendFactor = blend_factor(t.DestBlend);
        a.colorBlendOp = minus_one(t.BlendOp, VK_BLEND_OP_ADD);
        a.srcAlphaBlendFactor = blend_factor(t.SrcBlendAlpha);
        a.dstAlphaBlendFactor = blend_factor(t.DestBlendAlpha);
        a.alphaBlendOp = minus_one(t.BlendOpAlpha, VK_BLEND_OP_ADD);
        a.colorWriteMask = t.RenderTargetWriteMask & 0xf;
        color_formats[i] = g.rtv_formats[i] == DXGI_FORMAT_UNKNOWN ? VK_FORMAT_UNDEFINED : d.resolve_format(g.rtv_formats[i]);
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.logicOpEnable = g.blend.RenderTarget[0].LogicOpEnable && d.physical->features.logicOp;
    cb.logicOp = logic_op(g.blend.RenderTarget[0].LogicOp);
    cb.attachmentCount = g.num_render_targets;
    cb.pAttachments = attachments.data();

    std::vector<VkDynamicState> dynamic = {
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,  VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,     VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_DEPTH_BOUNDS,
    };
    if (!bindings.empty())   // dynamic strides oblige a vkCmdBindVertexBuffers2 before every draw
        dynamic.push_back(VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE);
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = static_cast<uint32_t>(dynamic.size());
    dyn.pDynamicStates = dynamic.data();

    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = g.num_render_targets;
    rendering.pColorAttachmentFormats = color_formats.data();
    if (ds_aspects & VK_IMAGE_ASPECT_DEPTH_BIT)
        rendering.depthAttachmentFormat = ds_format;
    if (ds_aspects & VK_IMAGE_ASPECT_STENCIL_BIT)
        rendering.stencilAttachmentFormat = ds_format;

    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
    ci.stageCount = static_cast<uint32_t>(stages.infos.size());
    ci.pStages = stages.infos.data();
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs_ci;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dyn;
    ci.layout = impl->layout;
    r = d.vk.vkCreateGraphicsPipelines(d.device, VK_NULL_HANDLE, 1, &ci, nullptr, &impl->pipeline);
    if (r != VK_SUCCESS) {
        log(LogLevel::warn, "vkCreateGraphicsPipelines failed: %d", r);
        return hresult_from_vk(r);
    }
    impl_ = std::move(impl);
    return S_OK;
}

} // namespace vgd
