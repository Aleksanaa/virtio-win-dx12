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
            if (offset + 8 <= size && std::memcmp(bytes + offset, "DXIL", 4) == 0)
                s->dxil_ = true;
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
