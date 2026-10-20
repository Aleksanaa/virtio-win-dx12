// SPDX-License-Identifier: MIT
// Shader container parsing and pipeline state capture. No GPU needed.
#include <vgd/pipeline.h>

#include <cstdio>
#include <cstring>
#include <vector>

#define CHECK(cond)                                                                                  \
    do {                                                                                             \
        if (!(cond)) {                                                                               \
            std::fprintf(stderr, "pipeline: FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            return 1;                                                                                \
        }                                                                                            \
    } while (0)

// A minimal container: header, one part offset, one "DXIL" part with 4 payload bytes.
static std::vector<uint8_t> make_container(const char* fourcc)
{
    std::vector<uint8_t> c(32 + 4 + 8 + 4, 0);
    std::memcpy(c.data(), "DXBC", 4);
    const uint32_t size = static_cast<uint32_t>(c.size()), parts = 1, offset = 36, part_size = 4;
    std::memcpy(&c[24], &size, 4);
    std::memcpy(&c[28], &parts, 4);
    std::memcpy(&c[32], &offset, 4);
    std::memcpy(&c[36], fourcc, 4);
    std::memcpy(&c[40], &part_size, 4);
    return c;
}

int main()
{
    auto dxil = make_container("DXIL");
    std::shared_ptr<const vgd::Shader> vs;
    CHECK(SUCCEEDED(vgd::Shader::from_code(vgd::ShaderStage::vertex, dxil.data(), &vs)));
    CHECK(vs->is_dxil() && vs->code().size() == dxil.size() && vs->stage() == vgd::ShaderStage::vertex);

    auto dxbc = make_container("SHEX");
    std::shared_ptr<const vgd::Shader> legacy;
    CHECK(SUCCEEDED(vgd::Shader::from_code(vgd::ShaderStage::pixel, dxbc.data(), &legacy)));
    CHECK(!legacy->is_dxil());

    // The bare DXIL program the DDI passes: {version, size in words, "DXIL", dxil version, offset, size} + bitcode.
    uint32_t program[8] = {0x00010060, 8, 0, 0x100, 16, 8, 0x0b17c0de, 0};
    std::memcpy(&program[2], "DXIL", 4);
    std::shared_ptr<const vgd::Shader> bare;
    CHECK(SUCCEEDED(vgd::Shader::from_code(vgd::ShaderStage::vertex, program, &bare)));
    CHECK(bare->is_dxil() && bare->format() == vgd::Shader::Format::dxil_program && bare->code().size() == 32);

    dxil[0] = 'X';
    dxil[4] = 0;   // and no plausible program length either
    std::shared_ptr<const vgd::Shader> bad;
    CHECK(FAILED(vgd::Shader::from_code(vgd::ShaderStage::vertex, dxil.data(), &bad)));

    D3D12_ROOT_SIGNATURE_DESC1 rs_desc{};
    std::unique_ptr<vgd::RootSignature> rs_unique;
    CHECK(SUCCEEDED(vgd::RootSignature::create(rs_desc, &rs_unique)));
    std::shared_ptr<const vgd::RootSignature> rs = std::move(rs_unique);

    vgd::GraphicsPipelineDesc g;
    g.root_signature = rs;
    g.vs = vs;
    g.num_render_targets = 1;
    g.rtv_formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    std::unique_ptr<vgd::PipelineState> pso;
    CHECK(SUCCEEDED(vgd::PipelineState::create_graphics(g, &pso)));
    CHECK(!pso->is_compute() && pso->graphics().vs == vs);

    // The pipeline keeps its shader alive after the caller drops it (the runtime destroys shaders right away).
    std::weak_ptr<const vgd::Shader> weak = vs;
    vs.reset();
    g.vs.reset();
    CHECK(!weak.expired());
    pso.reset();
    CHECK(weak.expired());

    g.vs = nullptr;
    CHECK(FAILED(vgd::PipelineState::create_graphics(g, &pso)));
    CHECK(FAILED(vgd::PipelineState::create_compute({rs, nullptr}, &pso)));

    std::fprintf(stderr, "pipeline: PASS\n");
    return 0;
}
