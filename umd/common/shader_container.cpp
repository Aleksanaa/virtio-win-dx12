// SPDX-License-Identifier: MIT
#include "shader_container.h"
#include "log.h"

#include <d3d12TokenizedProgramFormat.hpp>

#include <cstring>
#include <string>

namespace vgd::umd {

namespace {

constexpr uint32_t tag(const char (&s)[5])
{
    return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 |
           uint32_t(uint8_t(s[3])) << 24;
}

// D3D_NAME values with no D3D10_SB_NAME counterpart: what pixel shaders write.
constexpr uint32_t kNameTarget = 64, kNameDepth = 65;

// Program types, from the version token (DXBC) or ProgramVersion (DXIL): the same high 16 bits.
enum : uint32_t { kPixel = 0, kVertex = 1, kGeometry = 2, kHull = 3, kDomain = 4, kCompute = 5 };

struct Semantic {
    const char* name;
    UINT index;
};

// The SV_ name of a system value; null if it has none we know of. `nth` counts earlier entries of the same system
// value in the signature (clip and cull distances span registers).
const char* system_value_name(UINT sv, UINT nth, UINT* index)
{
    *index = 0;
    switch (sv) {
    case D3D10_SB_NAME_POSITION: return "SV_Position";
    case D3D10_SB_NAME_CLIP_DISTANCE: *index = nth; return "SV_ClipDistance";
    case D3D10_SB_NAME_CULL_DISTANCE: *index = nth; return "SV_CullDistance";
    case D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX: return "SV_RenderTargetArrayIndex";
    case D3D10_SB_NAME_VIEWPORT_ARRAY_INDEX: return "SV_ViewportArrayIndex";
    case D3D10_SB_NAME_VERTEX_ID: return "SV_VertexID";
    case D3D10_SB_NAME_PRIMITIVE_ID: return "SV_PrimitiveID";
    case D3D10_SB_NAME_INSTANCE_ID: return "SV_InstanceID";
    case D3D10_SB_NAME_IS_FRONT_FACE: return "SV_IsFrontFace";
    case D3D10_SB_NAME_SAMPLE_INDEX: return "SV_SampleIndex";
    // Tessellation factors: one per edge or inside, numbered as the HLSL arrays are.
    case D3D11_SB_NAME_FINAL_QUAD_U_EQ_0_EDGE_TESSFACTOR: return "SV_TessFactor";
    case D3D11_SB_NAME_FINAL_QUAD_V_EQ_0_EDGE_TESSFACTOR: *index = 1; return "SV_TessFactor";
    case D3D11_SB_NAME_FINAL_QUAD_U_EQ_1_EDGE_TESSFACTOR: *index = 2; return "SV_TessFactor";
    case D3D11_SB_NAME_FINAL_QUAD_V_EQ_1_EDGE_TESSFACTOR: *index = 3; return "SV_TessFactor";
    case D3D11_SB_NAME_FINAL_QUAD_U_INSIDE_TESSFACTOR: return "SV_InsideTessFactor";
    case D3D11_SB_NAME_FINAL_QUAD_V_INSIDE_TESSFACTOR: *index = 1; return "SV_InsideTessFactor";
    case D3D11_SB_NAME_FINAL_TRI_U_EQ_0_EDGE_TESSFACTOR: return "SV_TessFactor";
    case D3D11_SB_NAME_FINAL_TRI_V_EQ_0_EDGE_TESSFACTOR: *index = 1; return "SV_TessFactor";
    case D3D11_SB_NAME_FINAL_TRI_W_EQ_0_EDGE_TESSFACTOR: *index = 2; return "SV_TessFactor";
    case D3D11_SB_NAME_FINAL_TRI_INSIDE_TESSFACTOR: return "SV_InsideTessFactor";
    case D3D11_SB_NAME_FINAL_LINE_DETAIL_TESSFACTOR: *index = 1; return "SV_TessFactor";
    case D3D11_SB_NAME_FINAL_LINE_DENSITY_TESSFACTOR: return "SV_TessFactor";
    default: return nullptr;
    }
}

UINT first_component(BYTE mask)
{
    for (UINT c = 0; c < 4; c++)
        if (mask & (1u << c))
            return c;
    return 0;
}

struct Named {
    const char* name;
    UINT index;
    uint32_t sysval;   // D3D_NAME
};

// The semantic of entries[i] (see shader_container.h).
Named semantic_of(const std::vector<SignatureEntry>& entries, UINT i, bool pixel_output)
{
    const auto& e = entries[i];
    UINT nth = 0;
    for (UINT j = 0; j < i; j++)
        nth += entries[j].system_value == e.system_value;
    Named n{nullptr, 0, uint32_t(e.system_value)};
    if ((n.name = system_value_name(e.system_value, nth, &n.index)))
        return n;
    if (pixel_output && e.reg != ~0u)
        return {"SV_Target", e.reg, kNameTarget};
    if (pixel_output)
        return {"SV_Depth", 0, kNameDepth};   // oDepth and friends are declared by register type; the name is unread
    return {generic_semantic(e.stream), generic_semantic_index(e.reg, first_component(e.mask)), 0};
}

// One ISG1/OSG1/PSG1 part: count, 8, then per entry {stream, name offset, semantic index, system value, component
// type, register, mask | used mask << 8, min precision}, then the names; offsets count from the part's data.
std::vector<uint8_t> signature_part(const std::vector<SignatureEntry>& entries, bool pixel_output)
{
    const UINT count = static_cast<UINT>(entries.size());
    std::vector<uint32_t> words{count, 8};
    std::string names;
    std::vector<size_t> name_at(count);
    for (UINT i = 0; i < count; i++) {
        const auto& e = entries[i];
        const Named n = semantic_of(entries, i, pixel_output);
        name_at[i] = names.size();
        names += n.name;
        names += '\0';
        words.insert(words.end(), {e.stream, 0, n.index, n.sysval, e.component_type, e.reg,
                                   uint32_t(e.mask) | uint32_t(e.mask) << 8, e.min_precision});
    }
    const size_t names_offset = words.size() * 4;
    for (UINT i = 0; i < count; i++)
        words[2 + i * 8 + 1] = static_cast<uint32_t>(names_offset + name_at[i]);
    std::vector<uint8_t> part(names_offset + names.size());
    std::memcpy(part.data(), words.data(), names_offset);
    std::memcpy(part.data() + names_offset, names.data(), names.size());
    part.resize((part.size() + 3) & ~size_t(3));
    return part;
}

struct Part {
    uint32_t tag;
    std::vector<uint8_t> data;
};

// Header: "DXBC", checksum, version 1, total size, part count, part offsets; each part: tag, size, data.
void assemble(const std::vector<Part>& parts, DxbcChecksum checksum, std::vector<uint8_t>* out)
{
    size_t size = 32 + 4 * parts.size();
    for (const auto& p : parts)
        size += 8 + p.data.size();
    std::vector<uint8_t>& c = *out;
    c.assign(size, 0);
    auto put = [&](size_t at, uint32_t v) { std::memcpy(c.data() + at, &v, 4); };
    put(0, tag("DXBC"));
    put(20, 1);
    put(24, static_cast<uint32_t>(size));
    put(28, static_cast<uint32_t>(parts.size()));
    size_t at = 32 + 4 * parts.size();
    for (size_t i = 0; i < parts.size(); i++) {
        put(32 + 4 * i, static_cast<uint32_t>(at));
        put(at, parts[i].tag);
        put(at + 4, static_cast<uint32_t>(parts[i].data.size()));
        std::memcpy(c.data() + at + 8, parts[i].data.data(), parts[i].data.size());
        at += 8 + parts[i].data.size();
    }
    uint32_t sum[4];
    checksum(c.data(), c.size(), sum);
    std::memcpy(c.data() + 4, sum, sizeof(sum));
}

} // namespace

const char* generic_semantic(UINT stream)
{
    // Stream-output entries are matched by name alone, so geometry shader streams 1 to 3 keep names of their own.
    static const char* const names[] = {"VGD_R", "VGD_S1R", "VGD_S2R", "VGD_S3R"};
    return names[stream < 4 ? stream : 0];
}

bool output_semantic(const std::vector<SignatureEntry>& output, UINT stream, UINT reg, BYTE mask, const char** name,
                     UINT* index, UINT* first)
{
    for (UINT i = 0; i < output.size(); i++) {
        const auto& e = output[i];
        if (e.stream != stream || e.reg != reg || !(e.mask & mask))
            continue;
        const Named n = semantic_of(output, i, false);
        *name = n.name;
        *index = n.index;
        *first = first_component(e.mask);
        return true;
    }
    return false;
}

void build_input_signature_container(const std::vector<SignatureEntry>& input, DxbcChecksum checksum,
                                     std::vector<uint8_t>* out)
{
    assemble({{tag("ISG1"), signature_part(input, false)}}, checksum, out);
}

HRESULT build_shader_container(const UINT* code, const ShaderSignatures& s, DxbcChecksum checksum,
                               std::vector<uint8_t>* out)
{
    // Both program forms keep their size in 32-bit words in the second word:
    //   DXBC: {version token, length in words, tokens...}
    //   DXIL: {ProgramVersion, size in words, "DXIL", DXIL version, bitcode offset, bitcode size, bitcode}
    if (!code || code[1] < 2 || code[1] > (16u << 20))
        return E_INVALIDARG;
    const size_t program_size = size_t(code[1]) * 4;
    const bool dxil = program_size >= 24 && code[2] == tag("DXIL");
    const uint32_t type = code[0] >> 16;
    const uint32_t major = dxil ? 6 : (code[0] >> 4) & 0xf;
    if (type > kCompute) {
        VGD_WARN("shader: program type %u not supported (version 0x%08x)", type, code[0]);
        return E_INVALIDARG;
    }

    std::vector<Part> parts;
    if (type != kCompute) {
        parts.push_back({tag("ISG1"), signature_part(s.input, false)});
        parts.push_back({tag("OSG1"), signature_part(s.output, type == kPixel)});
        if (type == kHull || type == kDomain)
            parts.push_back({tag("PSG1"), signature_part(s.patch_constant, false)});
    }
    Part program{dxil ? tag("DXIL") : major >= 5 ? tag("SHEX") : tag("SHDR"), {}};
    program.data.resize(program_size);
    std::memcpy(program.data.data(), code, program_size);
    parts.push_back(std::move(program));
    assemble(parts, checksum, out);
    return S_OK;
}

} // namespace vgd::umd
