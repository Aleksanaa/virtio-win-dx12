// SPDX-License-Identifier: MIT
// Shaders, input layouts and the fixed-function state objects. Shaders arrive as bare programs with register-based
// signatures and are rebuilt into the containers DXVK takes (shader_container.h).
#include "device.h"
#include "shader_container.h"

namespace vgd::umd {

namespace {

// --- Shaders -----------------------------------------------------------------------------------------------

// The D3D11.1 runtime does not fill the stream field of the entries (D3D11_1DDIARG_SIGNATURE_ENTRY2 inserted it
// into padding): every entry is taken as stream 0.
std::vector<SignatureEntry> entries(const D3D11_1DDIARG_SIGNATURE_ENTRY* e, UINT count)
{
    std::vector<SignatureEntry> out;
    for (UINT i = 0; e && i < count; i++)
        out.push_back({static_cast<UINT>(e[i].SystemValue), e[i].Register, e[i].Mask, 0,
                       static_cast<UINT>(e[i].RegisterComponentType), static_cast<UINT>(e[i].MinPrecision)});
    return out;
}

ShaderSignatures stage_signatures(const D3D11_1DDIARG_STAGE_IO_SIGNATURES* s)
{
    ShaderSignatures out;
    if (s) {
        out.input = entries(s->pInputSignatureDeprecated, s->NumInputSignatureEntries);
        out.output = entries(s->pOutputSignatureDeprecated, s->NumOutputSignatureEntries);
    }
    return out;
}

ShaderSignatures tessellation_signatures(const D3D11_1DDIARG_TESSELLATION_IO_SIGNATURES* s)
{
    ShaderSignatures out;
    if (s) {
        out.input = entries(s->pInputSignatureDeprecated, s->NumInputSignatureEntries);
        out.output = entries(s->pOutputSignatureDeprecated, s->NumOutputSignatureEntries);
        out.patch_constant = entries(s->pPatchConstantSignatureDeprecated, s->NumPatchConstantSignatureEntries);
    }
    return out;
}

SIZE_T APIENTRY calc_private_shader_size(D3D10DDI_HDEVICE, const UINT*, const D3D11_1DDIARG_STAGE_IO_SIGNATURES*)
{
    return sizeof(ShaderObject);
}

SIZE_T APIENTRY calc_private_tessellation_shader_size(D3D10DDI_HDEVICE, const UINT*,
                                                      const D3D11_1DDIARG_TESSELLATION_IO_SIGNATURES*)
{
    return sizeof(ShaderObject);
}

SIZE_T APIENTRY calc_private_gs_so_size(D3D10DDI_HDEVICE, const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT*,
                                        const D3D11_1DDIARG_STAGE_IO_SIGNATURES*)
{
    return sizeof(ShaderObject);
}

enum class Stage { vertex, pixel, geometry, hull, domain, compute };

void create_shader(D3D10DDI_HDEVICE h, Stage stage, const UINT* code, const ShaderSignatures& s, D3D10DDI_HSHADER hs)
{
    Device* d = Device::from(h);
    auto* obj = new (hs.pDrvPrivate) ShaderObject;
    std::vector<uint8_t> c;
    HRESULT hr = build_shader_container(code, s, dxvk::dxbc_checksum, &c);
    if (SUCCEEDED(hr)) {
        auto** out = obj->shader.put();
        switch (stage) {
        case Stage::vertex:
            hr = d->d3d->CreateVertexShader(c.data(), c.size(), nullptr, reinterpret_cast<ID3D11VertexShader**>(out));
            break;
        case Stage::pixel:
            hr = d->d3d->CreatePixelShader(c.data(), c.size(), nullptr, reinterpret_cast<ID3D11PixelShader**>(out));
            break;
        case Stage::geometry:
            hr = d->d3d->CreateGeometryShader(c.data(), c.size(), nullptr,
                                              reinterpret_cast<ID3D11GeometryShader**>(out));
            break;
        case Stage::hull:
            hr = d->d3d->CreateHullShader(c.data(), c.size(), nullptr, reinterpret_cast<ID3D11HullShader**>(out));
            break;
        case Stage::domain:
            hr = d->d3d->CreateDomainShader(c.data(), c.size(), nullptr, reinterpret_cast<ID3D11DomainShader**>(out));
            break;
        case Stage::compute:
            hr = d->d3d->CreateComputeShader(c.data(), c.size(), nullptr,
                                             reinterpret_cast<ID3D11ComputeShader**>(out));
            break;
        }
    }
    if (FAILED(hr)) {
        VGD_WARN("Create shader (stage %d, version 0x%08x, %u words) failed: 0x%08lx", static_cast<int>(stage),
                 code ? code[0] : 0, code ? code[1] : 0, hr);
        d->set_error(E_OUTOFMEMORY);
    }
}

void APIENTRY create_vs(D3D10DDI_HDEVICE h, const UINT* code, D3D10DDI_HSHADER hs, D3D10DDI_HRTSHADER,
                        const D3D11_1DDIARG_STAGE_IO_SIGNATURES* s)
{
    create_shader(h, Stage::vertex, code, stage_signatures(s), hs);
}

void APIENTRY create_ps(D3D10DDI_HDEVICE h, const UINT* code, D3D10DDI_HSHADER hs, D3D10DDI_HRTSHADER,
                        const D3D11_1DDIARG_STAGE_IO_SIGNATURES* s)
{
    create_shader(h, Stage::pixel, code, stage_signatures(s), hs);
}

void APIENTRY create_gs(D3D10DDI_HDEVICE h, const UINT* code, D3D10DDI_HSHADER hs, D3D10DDI_HRTSHADER,
                        const D3D11_1DDIARG_STAGE_IO_SIGNATURES* s)
{
    create_shader(h, Stage::geometry, code, stage_signatures(s), hs);
}

void APIENTRY create_hs(D3D10DDI_HDEVICE h, const UINT* code, D3D10DDI_HSHADER hs, D3D10DDI_HRTSHADER,
                        const D3D11_1DDIARG_TESSELLATION_IO_SIGNATURES* s)
{
    create_shader(h, Stage::hull, code, tessellation_signatures(s), hs);
}

void APIENTRY create_ds(D3D10DDI_HDEVICE h, const UINT* code, D3D10DDI_HSHADER hs, D3D10DDI_HRTSHADER,
                        const D3D11_1DDIARG_TESSELLATION_IO_SIGNATURES* s)
{
    create_shader(h, Stage::domain, code, tessellation_signatures(s), hs);
}

void APIENTRY create_cs(D3D10DDI_HDEVICE h, const UINT* code, D3D10DDI_HSHADER hs, D3D10DDI_HRTSHADER)
{
    create_shader(h, Stage::compute, code, {}, hs);
}

// The DDI names stream-output components by output register; the API by the semantics of the output signature,
// which the container gives the same names (shader_container.h).
void APIENTRY create_gs_with_so(D3D10DDI_HDEVICE h, const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT* a,
                                D3D10DDI_HSHADER hs, D3D10DDI_HRTSHADER, const D3D11_1DDIARG_STAGE_IO_SIGNATURES* s)
{
    Device* d = Device::from(h);
    auto* obj = new (hs.pDrvPrivate) ShaderObject;
    if (!a->pShaderCode) {
        // Stream output of the vertex or domain program, with no geometry program to compile.
        VGD_WARN("CreateGeometryShaderWithStreamOutput without a geometry program: not supported yet");
        d->set_error(E_OUTOFMEMORY);
        return;
    }
    const ShaderSignatures sig = stage_signatures(s);
    std::vector<D3D11_SO_DECLARATION_ENTRY> decl;
    for (UINT i = 0; i < a->NumEntries; i++) {
        const auto& e = a->pOutputStreamDecl[i];
        D3D11_SO_DECLARATION_ENTRY o{};
        o.Stream = e.Stream;
        o.OutputSlot = static_cast<BYTE>(e.OutputSlot);
        UINT lo = 0, count = 0;
        for (UINT c = 0; c < 4; c++)
            if (e.RegisterMask & (1u << c)) {
                if (!count)
                    lo = c;
                count++;
            }
        o.ComponentCount = static_cast<BYTE>(count);
        UINT first = 0;
        if (e.RegisterIndex == ~0u || !output_semantic(sig.output, 0, e.RegisterIndex, e.RegisterMask,
                                                         &o.SemanticName, &o.SemanticIndex, &first)) {
            o.SemanticName = nullptr;   // a gap in the buffer
            o.SemanticIndex = 0;
        } else {
            o.StartComponent = static_cast<BYTE>(lo - first);
        }
        decl.push_back(o);
    }
    std::vector<uint8_t> c;
    HRESULT hr = build_shader_container(a->pShaderCode, sig, dxvk::dxbc_checksum, &c);
    if (SUCCEEDED(hr))
        hr = d->d3d->CreateGeometryShaderWithStreamOutput(
            c.data(), c.size(), decl.data(), static_cast<UINT>(decl.size()), a->BufferStridesInBytes, a->NumStrides,
            a->RasterizedStream, nullptr, reinterpret_cast<ID3D11GeometryShader**>(obj->shader.put()));
    if (FAILED(hr)) {
        VGD_WARN("CreateGeometryShaderWithStreamOutput (%u entries) failed: 0x%08lx", a->NumEntries, hr);
        d->set_error(E_OUTOFMEMORY);
    }
}

void APIENTRY destroy_shader(D3D10DDI_HDEVICE, D3D10DDI_HSHADER hs)
{
    object<ShaderObject>(hs)->~ShaderObject();
}

// --- Input layouts -----------------------------------------------------------------------------------------

SIZE_T APIENTRY calc_private_element_layout_size(D3D10DDI_HDEVICE, const D3D10DDIARG_CREATEELEMENTLAYOUT*)
{
    return sizeof(StateObject<ID3D11InputLayout>);
}

// The API names each element by semantic and matches it to the vertex shader's input signature; the DDI gives
// registers. Elements are named as the container names vertex inputs (shader_container.h), and the "shader" they are
// matched against is a container holding just that input signature.
void APIENTRY create_element_layout(D3D10DDI_HDEVICE h, const D3D10DDIARG_CREATEELEMENTLAYOUT* a,
                                    D3D10DDI_HELEMENTLAYOUT hl, D3D10DDI_HRTELEMENTLAYOUT)
{
    Device* d = Device::from(h);
    auto* obj = new (hl.pDrvPrivate) StateObject<ID3D11InputLayout>;
    std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
    std::vector<SignatureEntry> signature;
    for (UINT i = 0; i < a->NumElements; i++) {
        const auto& e = a->pVertexElements[i];
        D3D11_INPUT_ELEMENT_DESC o{};
        // Vertex inputs are not packed: each starts at component 0 of its register.
        o.SemanticName = generic_semantic(0);
        o.SemanticIndex = generic_semantic_index(e.InputRegister, 0);
        o.Format = e.Format;
        o.InputSlot = e.InputSlot;
        o.AlignedByteOffset = e.AlignedByteOffset;
        o.InputSlotClass = static_cast<D3D11_INPUT_CLASSIFICATION>(e.InputSlotClass);   // same values
        o.InstanceDataStepRate = e.InstanceDataStepRate;
        elements.push_back(o);
        // D3D10_SB_REGISTER_COMPONENT_FLOAT32: the type is the format's, which the layout gives.
        signature.push_back({0, e.InputRegister, 0xf, 0, 3, 0});
    }
    std::vector<uint8_t> c;
    build_input_signature_container(signature, dxvk::dxbc_checksum, &c);
    HRESULT hr = d->d3d->CreateInputLayout(elements.data(), static_cast<UINT>(elements.size()), c.data(), c.size(),
                                           obj->state.put());
    if (FAILED(hr)) {
        VGD_WARN("CreateElementLayout (%u elements) failed: 0x%08lx", a->NumElements, hr);
        d->set_error(E_OUTOFMEMORY);
    }
}

template <class T, class H>
void APIENTRY destroy_state(D3D10DDI_HDEVICE, H hs)
{
    object<StateObject<T>>(hs)->~StateObject<T>();
}

// --- Fixed-function state ----------------------------------------------------------------------------------
// The DDI's blend, comparison, stencil, fill, cull, filter and address values are the API's.

SIZE_T APIENTRY calc_private_blend_state_size(D3D10DDI_HDEVICE, const D3D11_1_DDI_BLEND_DESC*)
{
    return sizeof(StateObject<ID3D11BlendState1>);
}

void APIENTRY create_blend_state(D3D10DDI_HDEVICE h, const D3D11_1_DDI_BLEND_DESC* a, D3D10DDI_HBLENDSTATE hb,
                                 D3D10DDI_HRTBLENDSTATE)
{
    Device* d = Device::from(h);
    auto* obj = new (hb.pDrvPrivate) StateObject<ID3D11BlendState1>;
    D3D11_BLEND_DESC1 desc{a->AlphaToCoverageEnable, a->IndependentBlendEnable, {}};
    for (UINT i = 0; i < 8; i++) {
        const auto& s = a->RenderTarget[i];
        desc.RenderTarget[i] = {s.BlendEnable,
                                s.LogicOpEnable,
                                static_cast<D3D11_BLEND>(s.SrcBlend),
                                static_cast<D3D11_BLEND>(s.DestBlend),
                                static_cast<D3D11_BLEND_OP>(s.BlendOp),
                                static_cast<D3D11_BLEND>(s.SrcBlendAlpha),
                                static_cast<D3D11_BLEND>(s.DestBlendAlpha),
                                static_cast<D3D11_BLEND_OP>(s.BlendOpAlpha),
                                static_cast<D3D11_LOGIC_OP>(s.LogicOp),
                                s.RenderTargetWriteMask};
    }
    HRESULT hr = d->d3d->CreateBlendState1(&desc, obj->state.put());
    if (FAILED(hr)) {
        VGD_WARN("CreateBlendState failed: 0x%08lx", hr);
        d->set_error(E_OUTOFMEMORY);
    }
}

SIZE_T APIENTRY calc_private_depth_stencil_state_size(D3D10DDI_HDEVICE, const D3D10_DDI_DEPTH_STENCIL_DESC*)
{
    return sizeof(StateObject<ID3D11DepthStencilState>);
}

D3D11_DEPTH_STENCILOP_DESC stencil_face(BOOL enabled, const D3D10_DDI_DEPTH_STENCILOP_DESC& f)
{
    // A face without stencil testing passes and keeps what is there.
    if (!enabled)
        return {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
    return {static_cast<D3D11_STENCIL_OP>(f.StencilFailOp), static_cast<D3D11_STENCIL_OP>(f.StencilDepthFailOp),
            static_cast<D3D11_STENCIL_OP>(f.StencilPassOp), static_cast<D3D11_COMPARISON_FUNC>(f.StencilFunc)};
}

void APIENTRY create_depth_stencil_state(D3D10DDI_HDEVICE h, const D3D10_DDI_DEPTH_STENCIL_DESC* a,
                                         D3D10DDI_HDEPTHSTENCILSTATE hs, D3D10DDI_HRTDEPTHSTENCILSTATE)
{
    Device* d = Device::from(h);
    auto* obj = new (hs.pDrvPrivate) StateObject<ID3D11DepthStencilState>;
    D3D11_DEPTH_STENCIL_DESC desc{a->DepthEnable,
                                  static_cast<D3D11_DEPTH_WRITE_MASK>(a->DepthWriteMask),
                                  static_cast<D3D11_COMPARISON_FUNC>(a->DepthFunc),
                                  a->StencilEnable,
                                  a->StencilReadMask,
                                  a->StencilWriteMask,
                                  stencil_face(a->FrontEnable, a->FrontFace),
                                  stencil_face(a->BackEnable, a->BackFace)};
    HRESULT hr = d->d3d->CreateDepthStencilState(&desc, obj->state.put());
    if (FAILED(hr)) {
        VGD_WARN("CreateDepthStencilState failed: 0x%08lx", hr);
        d->set_error(E_OUTOFMEMORY);
    }
}

SIZE_T APIENTRY calc_private_rasterizer_state_size(D3D10DDI_HDEVICE, const D3D11_1_DDI_RASTERIZER_DESC*)
{
    return sizeof(StateObject<ID3D11RasterizerState1>);
}

void APIENTRY create_rasterizer_state(D3D10DDI_HDEVICE h, const D3D11_1_DDI_RASTERIZER_DESC* a,
                                      D3D10DDI_HRASTERIZERSTATE hs, D3D10DDI_HRTRASTERIZERSTATE)
{
    Device* d = Device::from(h);
    auto* obj = new (hs.pDrvPrivate) StateObject<ID3D11RasterizerState1>;
    D3D11_RASTERIZER_DESC1 desc{static_cast<D3D11_FILL_MODE>(a->FillMode),
                                static_cast<D3D11_CULL_MODE>(a->CullMode),
                                a->FrontCounterClockwise,
                                a->DepthBias,
                                a->DepthBiasClamp,
                                a->SlopeScaledDepthBias,
                                a->DepthClipEnable,
                                a->ScissorEnable,
                                a->MultisampleEnable,
                                a->AntialiasedLineEnable,
                                a->ForcedSampleCount};
    HRESULT hr = d->d3d->CreateRasterizerState1(&desc, obj->state.put());
    if (FAILED(hr)) {
        VGD_WARN("CreateRasterizerState failed: 0x%08lx", hr);
        d->set_error(E_OUTOFMEMORY);
    }
}

SIZE_T APIENTRY calc_private_sampler_size(D3D10DDI_HDEVICE, const D3D10_DDI_SAMPLER_DESC*)
{
    return sizeof(StateObject<ID3D11SamplerState>);
}

void APIENTRY create_sampler(D3D10DDI_HDEVICE h, const D3D10_DDI_SAMPLER_DESC* a, D3D10DDI_HSAMPLER hs,
                             D3D10DDI_HRTSAMPLER)
{
    Device* d = Device::from(h);
    auto* obj = new (hs.pDrvPrivate) StateObject<ID3D11SamplerState>;
    D3D11_SAMPLER_DESC desc{static_cast<D3D11_FILTER>(a->Filter),
                            static_cast<D3D11_TEXTURE_ADDRESS_MODE>(a->AddressU),
                            static_cast<D3D11_TEXTURE_ADDRESS_MODE>(a->AddressV),
                            static_cast<D3D11_TEXTURE_ADDRESS_MODE>(a->AddressW),
                            a->MipLODBias,
                            a->MaxAnisotropy,
                            static_cast<D3D11_COMPARISON_FUNC>(a->ComparisonFunc),
                            {a->BorderColor[0], a->BorderColor[1], a->BorderColor[2], a->BorderColor[3]},
                            a->MinLOD,
                            a->MaxLOD};
    HRESULT hr = d->d3d->CreateSamplerState(&desc, obj->state.put());
    if (FAILED(hr)) {
        VGD_WARN("CreateSampler (filter 0x%x) failed: 0x%08lx", a->Filter, hr);
        d->set_error(E_OUTOFMEMORY);
    }
}

} // namespace

void fill_pipeline_ddi(D3D11_1DDI_DEVICEFUNCS& t)
{
    t.pfnCalcPrivateShaderSize = calc_private_shader_size;
    t.pfnCalcPrivateTessellationShaderSize = calc_private_tessellation_shader_size;
    t.pfnCalcPrivateGeometryShaderWithStreamOutput = calc_private_gs_so_size;
    t.pfnCreateVertexShader = create_vs;
    t.pfnCreatePixelShader = create_ps;
    t.pfnCreateGeometryShader = create_gs;
    t.pfnCreateHullShader = create_hs;
    t.pfnCreateDomainShader = create_ds;
    t.pfnCreateComputeShader = create_cs;
    t.pfnCreateGeometryShaderWithStreamOutput = create_gs_with_so;
    t.pfnDestroyShader = destroy_shader;
    t.pfnCalcPrivateElementLayoutSize = calc_private_element_layout_size;
    t.pfnCreateElementLayout = create_element_layout;
    t.pfnDestroyElementLayout = destroy_state<ID3D11InputLayout, D3D10DDI_HELEMENTLAYOUT>;
    t.pfnCalcPrivateBlendStateSize = calc_private_blend_state_size;
    t.pfnCreateBlendState = create_blend_state;
    t.pfnDestroyBlendState = destroy_state<ID3D11BlendState1, D3D10DDI_HBLENDSTATE>;
    t.pfnCalcPrivateDepthStencilStateSize = calc_private_depth_stencil_state_size;
    t.pfnCreateDepthStencilState = create_depth_stencil_state;
    t.pfnDestroyDepthStencilState = destroy_state<ID3D11DepthStencilState, D3D10DDI_HDEPTHSTENCILSTATE>;
    t.pfnCalcPrivateRasterizerStateSize = calc_private_rasterizer_state_size;
    t.pfnCreateRasterizerState = create_rasterizer_state;
    t.pfnDestroyRasterizerState = destroy_state<ID3D11RasterizerState1, D3D10DDI_HRASTERIZERSTATE>;
    t.pfnCalcPrivateSamplerSize = calc_private_sampler_size;
    t.pfnCreateSampler = create_sampler;
    t.pfnDestroySampler = destroy_state<ID3D11SamplerState, D3D10DDI_HSAMPLER>;
}

} // namespace vgd::umd
