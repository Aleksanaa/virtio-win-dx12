// SPDX-License-Identifier: MIT
// DXIL -> SPIR-V through dxil-spirv, with root parameters mapped onto the root data layout (root_signature.h).
#include "internal.h"

#if VGD_WITH_DXIL_SPIRV
#include <dxil_spirv_c.h>
#endif

#include <cstring>

namespace vgd {

VkShaderStageFlagBits vk_stage(ShaderStage stage)
{
    switch (stage) {
    case ShaderStage::vertex: return VK_SHADER_STAGE_VERTEX_BIT;
    case ShaderStage::hull: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    case ShaderStage::domain: return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    case ShaderStage::geometry: return VK_SHADER_STAGE_GEOMETRY_BIT;
    case ShaderStage::pixel: return VK_SHADER_STAGE_FRAGMENT_BIT;
    case ShaderStage::compute: return VK_SHADER_STAGE_COMPUTE_BIT;
    case ShaderStage::amplification: return VK_SHADER_STAGE_TASK_BIT_EXT;
    case ShaderStage::mesh: return VK_SHADER_STAGE_MESH_BIT_EXT;
    }
    return VK_SHADER_STAGE_ALL;
}

#if VGD_WITH_DXIL_SPIRV

namespace {

// Where a shader's bindings go:
//  - root constants and root CBV/SRV/UAV: push constants (root data, see root_signature.h);
//  - descriptor tables: set 0 (CBV/SRV/UAV heap) or set 1 (sampler heap), binding 0, at the table's offset from
//    the root data plus the register's offset in the table;
//  - static samplers: set 2, one binding each.
struct Mapping {
    const RootSignature& rs;
    ShaderStage stage;
    bool bindless;
    const char* failure = nullptr;   // the first binding we could not map
};

bool visible(D3D12_SHADER_VISIBILITY v, ShaderStage s)
{
    switch (v) {
    case D3D12_SHADER_VISIBILITY_ALL: return true;
    case D3D12_SHADER_VISIBILITY_VERTEX: return s == ShaderStage::vertex;
    case D3D12_SHADER_VISIBILITY_HULL: return s == ShaderStage::hull;
    case D3D12_SHADER_VISIBILITY_DOMAIN: return s == ShaderStage::domain;
    case D3D12_SHADER_VISIBILITY_GEOMETRY: return s == ShaderStage::geometry;
    case D3D12_SHADER_VISIBILITY_PIXEL: return s == ShaderStage::pixel;
    case D3D12_SHADER_VISIBILITY_AMPLIFICATION: return s == ShaderStage::amplification;
    case D3D12_SHADER_VISIBILITY_MESH: return s == ShaderStage::mesh;
    }
    return false;
}

// The root parameter of `type` bound to register (space, index) for this stage, or null.
const RootSignature::Parameter* find_root(const Mapping& m, D3D12_ROOT_PARAMETER_TYPE type,
                                          const dxil_spv_d3d_binding& b)
{
    const auto& desc = m.rs.desc();
    for (uint32_t i = 0; i < desc.NumParameters; i++) {
        const auto& p = desc.pParameters[i];
        if (p.ParameterType != type || !visible(p.ShaderVisibility, m.stage))
            continue;
        const bool constants = type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        const UINT reg = constants ? p.Constants.ShaderRegister : p.Descriptor.ShaderRegister;
        const UINT space = constants ? p.Constants.RegisterSpace : p.Descriptor.RegisterSpace;
        if (reg == b.register_index && space == b.register_space)
            return &m.rs.parameters()[i];
    }
    return nullptr;
}

// A descriptor table range of `type` containing the binding: fills the heap binding and returns true.
bool find_table(const Mapping& m, D3D12_DESCRIPTOR_RANGE_TYPE type, const dxil_spv_d3d_binding& b,
                dxil_spv_vulkan_binding* out)
{
    if (!m.bindless)
        return false;
    const auto& desc = m.rs.desc();
    const uint64_t count = b.range_size == ~0u ? 1 : b.range_size;
    for (uint32_t i = 0; i < desc.NumParameters; i++) {
        const auto& p = desc.pParameters[i];
        if (p.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE || !visible(p.ShaderVisibility, m.stage))
            continue;
        for (uint32_t j = 0; j < p.DescriptorTable.NumDescriptorRanges; j++) {
            const auto& r = p.DescriptorTable.pDescriptorRanges[j];
            if (r.RangeType != type || r.RegisterSpace != b.register_space || b.register_index < r.BaseShaderRegister)
                continue;
            if (r.NumDescriptors != UINT_MAX && b.register_index + count > uint64_t(r.BaseShaderRegister) + r.NumDescriptors)
                continue;
            *out = {};
            out->set = type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER ? 1 : 0;
            out->binding = 0;
            out->root_constant_index = m.rs.parameters()[i].index;   // the word holding the table's heap offset
            out->bindless.use_heap = DXIL_SPV_TRUE;
            out->bindless.heap_root_offset = r.OffsetInDescriptorsFromTableStart + b.register_index - r.BaseShaderRegister;
            return true;
        }
    }
    return false;
}

dxil_spv_bool unmapped(Mapping& m, const char* what, const dxil_spv_d3d_binding& b)
{
    if (!m.failure)
        m.failure = what;
    log(LogLevel::warn, "%s shader: %s register %u space %u is in no root parameter, table or static sampler%s",
        stage_name(m.stage), what, b.register_index, b.register_space,
        m.bindless ? "" : " (descriptor tables need mutable descriptors)");
    return DXIL_SPV_FALSE;
}

bool is_untyped_buffer(dxil_spv_resource_kind k)
{
    return k == DXIL_SPV_RESOURCE_KIND_RAW_BUFFER || k == DXIL_SPV_RESOURCE_KIND_STRUCTURED_BUFFER;
}

dxil_spv_bool map_cbv(void* user, const dxil_spv_d3d_binding* b, dxil_spv_cbv_vulkan_binding* out)
{
    auto& m = *static_cast<Mapping*>(user);
    *out = {};
    if (auto* p = find_root(m, D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, *b)) {
        out->push_constant = DXIL_SPV_TRUE;
        out->vulkan.push_constant.offset_in_words = p->index;
        return DXIL_SPV_TRUE;
    }
    if (auto* p = find_root(m, D3D12_ROOT_PARAMETER_TYPE_CBV, *b)) {
        out->vulkan.uniform_binding.descriptor_type = DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_BUFFER_DEVICE_ADDRESS;
        out->vulkan.uniform_binding.root_constant_index = p->index;
        return DXIL_SPV_TRUE;
    }
    if (find_table(m, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, *b, &out->vulkan.uniform_binding))
        return DXIL_SPV_TRUE;   // a uniform buffer in the heap
    return unmapped(m, "CBV", *b);
}

dxil_spv_bool map_srv(void* user, const dxil_spv_d3d_binding* b, dxil_spv_srv_vulkan_binding* out)
{
    auto& m = *static_cast<Mapping*>(user);
    *out = {};
    if (auto* p = find_root(m, D3D12_ROOT_PARAMETER_TYPE_SRV, *b)) {
        out->buffer_binding.descriptor_type = DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_BUFFER_DEVICE_ADDRESS;
        out->buffer_binding.root_constant_index = p->index;
        return DXIL_SPV_TRUE;
    }
    if (find_table(m, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, *b, &out->buffer_binding)) {
        // Raw and structured buffers are storage buffers in the heap; typed buffers texel buffers.
        if (is_untyped_buffer(b->kind))
            out->buffer_binding.descriptor_type = DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_SSBO;
        return DXIL_SPV_TRUE;
    }
    return unmapped(m, "SRV", *b);
}

dxil_spv_bool map_uav(void* user, const dxil_spv_uav_d3d_binding* b, dxil_spv_uav_vulkan_binding* out)
{
    auto& m = *static_cast<Mapping*>(user);
    *out = {};
    if (b->has_counter) {
        log(LogLevel::warn, "%s shader: UAV u%u space %u has a counter; UAV counters are not supported yet",
            stage_name(m.stage), b->d3d_binding.register_index, b->d3d_binding.register_space);
        return unmapped(m, "UAV with counter", b->d3d_binding);
    }
    if (auto* p = find_root(m, D3D12_ROOT_PARAMETER_TYPE_UAV, b->d3d_binding)) {
        out->buffer_binding.descriptor_type = DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_BUFFER_DEVICE_ADDRESS;
        out->buffer_binding.root_constant_index = p->index;
        return DXIL_SPV_TRUE;
    }
    if (find_table(m, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, b->d3d_binding, &out->buffer_binding)) {
        if (is_untyped_buffer(b->d3d_binding.kind))
            out->buffer_binding.descriptor_type = DXIL_SPV_VULKAN_DESCRIPTOR_TYPE_SSBO;
        return DXIL_SPV_TRUE;
    }
    return unmapped(m, "UAV", b->d3d_binding);
}

dxil_spv_bool map_sampler(void* user, const dxil_spv_d3d_binding* b, dxil_spv_vulkan_binding* out)
{
    auto& m = *static_cast<Mapping*>(user);
    const auto& desc = m.rs.desc();
    for (uint32_t i = 0; i < desc.NumStaticSamplers; i++) {
        const auto& s = desc.pStaticSamplers[i];
        if (s.ShaderRegister == b->register_index && s.RegisterSpace == b->register_space &&
            visible(s.ShaderVisibility, m.stage)) {
            *out = {};
            out->set = kStaticSamplerSet;
            out->binding = i;
            return DXIL_SPV_TRUE;
        }
    }
    if (find_table(m, D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, *b, out))
        return DXIL_SPV_TRUE;
    return unmapped(m, "sampler", *b);
}

void forward_log(void*, dxil_spv_log_level level, const char* msg)
{
    log(level == DXIL_SPV_LOG_LEVEL_ERROR ? LogLevel::warn : LogLevel::debug, "dxil-spirv: %s", msg);
}

template <class T>
void add_option(dxil_spv_converter c, dxil_spv_option type, T value)
{
    value.base.type = type;
    if (dxil_spv_converter_add_option(c, &value.base) != DXIL_SPV_SUCCESS)
        log(LogLevel::warn, "dxil-spirv: option %d not supported", type);
}

} // namespace

HRESULT translate_shader(const Shader& shader, const RootSignature& rs, const ShaderOptions& options,
                         std::vector<uint32_t>* spirv, std::string* entry_point)
{
    if (!shader.is_dxil()) {
        log(LogLevel::warn, "%s shader is DXBC; only DXIL is supported", stage_name(shader.stage()));
        return E_NOTIMPL;
    }
    const auto bitcode = shader.bitcode();
    if (bitcode.empty())
        return E_INVALIDARG;
    dxil_spv_set_thread_log_callback(forward_log, nullptr);

    dxil_spv_parsed_blob blob = nullptr;
    if (dxil_spv_parse_dxil(bitcode.data(), bitcode.size(), &blob) != DXIL_SPV_SUCCESS) {
        log(LogLevel::warn, "%s shader: dxil-spirv cannot parse the bitcode", stage_name(shader.stage()));
        return E_INVALIDARG;
    }
    dxil_spv_converter conv = nullptr;
    if (dxil_spv_create_converter(blob, &conv) != DXIL_SPV_SUCCESS) {
        dxil_spv_parsed_blob_free(blob);
        return E_OUTOFMEMORY;
    }

    Mapping m{rs, shader.stage(), options.bindless};
    dxil_spv_converter_set_root_constant_word_count(conv, rs.root_word_count());
    dxil_spv_converter_set_root_descriptor_count(conv, rs.root_descriptor_count());
    dxil_spv_converter_set_cbv_remapper(conv, map_cbv, &m);
    dxil_spv_converter_set_srv_remapper(conv, map_srv, &m);
    dxil_spv_converter_set_uav_remapper(conv, map_uav, &m);
    dxil_spv_converter_set_sampler_remapper(conv, map_sampler, &m);
    add_option(conv, DXIL_SPV_OPTION_PHYSICAL_STORAGE_BUFFER, dxil_spv_option_physical_storage_buffer{{}, DXIL_SPV_TRUE});
    add_option(conv, DXIL_SPV_OPTION_SSBO_ALIGNMENT, dxil_spv_option_ssbo_alignment{{}, options.ssbo_alignment});
    add_option(conv, DXIL_SPV_OPTION_TYPED_UAV_READ_WITHOUT_FORMAT,
               dxil_spv_option_typed_uav_read_without_format{{}, options.typed_uav_read_without_format});

    HRESULT hr = S_OK;
    dxil_spv_compiled_spirv out{};
    if (dxil_spv_converter_run(conv) != DXIL_SPV_SUCCESS ||
        dxil_spv_converter_get_compiled_spirv(conv, &out) != DXIL_SPV_SUCCESS || !out.size) {
        log(LogLevel::warn, "%s shader: translation failed%s%s", stage_name(shader.stage()),
            m.failure ? " at an unmapped " : "", m.failure ? m.failure : "");
        hr = E_INVALIDARG;
    } else {
        spirv->resize(out.size / 4);
        std::memcpy(spirv->data(), out.data, out.size);
        *entry_point = "main";   // dxil-spirv names the SPIR-V entry point main
    }
    dxil_spv_converter_free(conv);
    dxil_spv_parsed_blob_free(blob);
    return hr;
}

#else

HRESULT translate_shader(const Shader& shader, const RootSignature&, const ShaderOptions&, std::vector<uint32_t>*,
                         std::string*)
{
    log(LogLevel::warn, "%s shader: built without dxil-spirv", stage_name(shader.stage()));
    return E_NOTIMPL;
}

#endif

} // namespace vgd
