// SPDX-License-Identifier: MIT
// Shaders, fixed-function state objects, input layouts and pipeline state objects. Everything is captured into
// core objects; CreatePipelineState translates the shaders and builds the Vulkan pipeline.
#include "device.h"
#include "log.h"
#include "objects.h"

#include <new>

namespace vgd::umd {

namespace {

// Void creation DDIs report failure through the runtime's error callback.
void set_error(D3D12DDI_HDEVICE h, HRESULT hr)
{
    Device* d = Device::from(h);
    d->um_callbacks->pfnSetErrorCb(d->rt_device, hr);
}

// --- Shaders -----------------------------------------------------------------------------------------------

SIZE_T APIENTRY calc_private_shader_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_SHADER_0026*)
{
    return sizeof(ShaderObject);
}

template <vgd::ShaderStage Stage>
void APIENTRY create_shader(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_SHADER_0026* args, D3D12DDI_HSHADER hs)
{
    auto* obj = new (hs.pDrvPrivate) ShaderObject;
    HRESULT hr = vgd::Shader::from_code(Stage, args->pShaderCode, &obj->shader);
    if (FAILED(hr)) {
        const UINT* c = args->pShaderCode;
        VGD_WARN("Create %s shader: unrecognized code (0x%08lx), first words %08x %08x %08x %08x", vgd::stage_name(Stage),
                 hr, c[0], c[1], c[2], c[3]);
        set_error(h, E_OUTOFMEMORY);
        return;
    }
    VGD_VERBOSE("Create %s shader: %u bytes, %s", vgd::stage_name(Stage),
                static_cast<unsigned>(obj->shader->code().size()), obj->shader->is_dxil() ? "DXIL" : "DXBC");
}

void APIENTRY destroy_shader(D3D12DDI_HDEVICE, D3D12DDI_HSHADER hs)
{
    object<ShaderObject>(hs)->~ShaderObject();
}

// --- Input layouts -----------------------------------------------------------------------------------------

SIZE_T APIENTRY calc_private_element_layout_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATEELEMENTLAYOUT_0010*)
{
    return sizeof(ElementLayoutObject);
}

void APIENTRY create_element_layout(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATEELEMENTLAYOUT_0010* args,
                                    D3D12DDI_HELEMENTLAYOUT hl)
{
    auto* obj = new (hl.pDrvPrivate) ElementLayoutObject;
    obj->elements.reserve(args->NumElements);
    for (UINT i = 0; i < args->NumElements; i++) {
        const auto& e = args->pVertexElements[i];
        obj->elements.push_back({e.InputRegister, e.InputSlot, e.AlignedByteOffset, e.Format,
                                 e.InputSlotClass == D3D12DDI_INPUT_CLASSIFICIATION_PER_INSTANCE_DATA,   // sic, WDK spelling
                                 e.InstanceDataStepRate});
    }
}

void APIENTRY destroy_element_layout(D3D12DDI_HDEVICE, D3D12DDI_HELEMENTLAYOUT hl)
{
    object<ElementLayoutObject>(hl)->~ElementLayoutObject();
}

// --- Fixed-function states ---------------------------------------------------------------------------------
// The DDI structs carry extra driver fields (library references, ScissorEnable, per-face stencil enables), so they
// are converted field by field into the API structs core keeps.

SIZE_T APIENTRY calc_private_blend_state_size(D3D12DDI_HDEVICE, const D3D12DDI_BLEND_DESC_0010*)
{
    return sizeof(BlendStateObject);
}

void APIENTRY create_blend_state(D3D12DDI_HDEVICE, const D3D12DDI_BLEND_DESC_0010* d, D3D12DDI_HBLENDSTATE hb)
{
    static_assert(sizeof(D3D12DDI_RENDER_TARGET_BLEND_DESC) == sizeof(D3D12_RENDER_TARGET_BLEND_DESC));
    D3D12_BLEND_DESC b{};
    b.AlphaToCoverageEnable = d->AlphaToCoverageEnable;
    b.IndependentBlendEnable = d->IndependentBlendEnable;
    for (int i = 0; i < 8; i++) {
        const auto& s = d->RenderTarget[i];
        auto& t = b.RenderTarget[i];
        t.BlendEnable = s.BlendEnable;
        t.LogicOpEnable = s.LogicOpEnable;
        t.SrcBlend = static_cast<D3D12_BLEND>(s.SrcBlend);
        t.DestBlend = static_cast<D3D12_BLEND>(s.DestBlend);
        t.BlendOp = static_cast<D3D12_BLEND_OP>(s.BlendOp);
        t.SrcBlendAlpha = static_cast<D3D12_BLEND>(s.SrcBlendAlpha);
        t.DestBlendAlpha = static_cast<D3D12_BLEND>(s.DestBlendAlpha);
        t.BlendOpAlpha = static_cast<D3D12_BLEND_OP>(s.BlendOpAlpha);
        t.LogicOp = static_cast<D3D12_LOGIC_OP>(s.LogicOp);
        t.RenderTargetWriteMask = s.RenderTargetWriteMask;
    }
    new (hb.pDrvPrivate) BlendStateObject{b};
}

void APIENTRY destroy_blend_state(D3D12DDI_HDEVICE, D3D12DDI_HBLENDSTATE hb)
{
    object<BlendStateObject>(hb)->~BlendStateObject();
}

SIZE_T APIENTRY calc_private_rasterizer_state_size(D3D12DDI_HDEVICE, const D3D12DDI_RASTERIZER_DESC_0010*)
{
    return sizeof(RasterizerStateObject);
}

void APIENTRY create_rasterizer_state(D3D12DDI_HDEVICE, const D3D12DDI_RASTERIZER_DESC_0010* d,
                                      D3D12DDI_HRASTERIZERSTATE hr_)
{
    D3D12_RASTERIZER_DESC r{};
    r.FillMode = static_cast<D3D12_FILL_MODE>(d->FillMode);
    r.CullMode = static_cast<D3D12_CULL_MODE>(d->CullMode);
    r.FrontCounterClockwise = d->FrontCounterClockwise;
    r.DepthBias = d->DepthBias;
    r.DepthBiasClamp = d->DepthBiasClamp;
    r.SlopeScaledDepthBias = d->SlopeScaledDepthBias;
    r.DepthClipEnable = d->DepthClipEnable;
    r.MultisampleEnable = d->MultisampleEnable;
    r.AntialiasedLineEnable = d->AntialiasedLineEnable;
    r.ForcedSampleCount = d->ForcedSampleCount;
    r.ConservativeRaster = static_cast<D3D12_CONSERVATIVE_RASTERIZATION_MODE>(d->ConservativeRasterizationMode);
    // ScissorEnable has no API equivalent: D3D12 always scissors.
    new (hr_.pDrvPrivate) RasterizerStateObject{r};
}

void APIENTRY destroy_rasterizer_state(D3D12DDI_HDEVICE, D3D12DDI_HRASTERIZERSTATE hr_)
{
    object<RasterizerStateObject>(hr_)->~RasterizerStateObject();
}

SIZE_T APIENTRY calc_private_depth_stencil_state_size(D3D12DDI_HDEVICE, const D3D12DDI_DEPTH_STENCIL_DESC_0025*)
{
    return sizeof(DepthStencilStateObject);
}

D3D12_DEPTH_STENCILOP_DESC stencil_op(const D3D12DDI_DEPTH_STENCILOP_DESC& s)
{
    return {static_cast<D3D12_STENCIL_OP>(s.StencilFailOp), static_cast<D3D12_STENCIL_OP>(s.StencilDepthFailOp),
            static_cast<D3D12_STENCIL_OP>(s.StencilPassOp), static_cast<D3D12_COMPARISON_FUNC>(s.StencilFunc)};
}

void APIENTRY create_depth_stencil_state(D3D12DDI_HDEVICE, const D3D12DDI_DEPTH_STENCIL_DESC_0025* d,
                                         D3D12DDI_HDEPTHSTENCILSTATE hd)
{
    D3D12_DEPTH_STENCIL_DESC1 ds{};
    ds.DepthEnable = d->DepthEnable;
    ds.DepthWriteMask = static_cast<D3D12_DEPTH_WRITE_MASK>(d->DepthWriteMask);
    ds.DepthFunc = static_cast<D3D12_COMPARISON_FUNC>(d->DepthFunc);
    ds.StencilEnable = d->StencilEnable;
    ds.StencilReadMask = d->StencilReadMask;
    ds.StencilWriteMask = d->StencilWriteMask;
    ds.FrontFace = stencil_op(d->FrontFace);
    ds.BackFace = stencil_op(d->BackFace);
    ds.DepthBoundsTestEnable = d->DepthBoundsTestEnable;
    if (d->StencilEnable && !(d->FrontEnable && d->BackEnable))
        VGD_WARN("CreateDepthStencilState: one-sided stencil (front %d, back %d) not handled yet", d->FrontEnable,
                 d->BackEnable);
    new (hd.pDrvPrivate) DepthStencilStateObject{ds};
}

void APIENTRY destroy_depth_stencil_state(D3D12DDI_HDEVICE, D3D12DDI_HDEPTHSTENCILSTATE hd)
{
    object<DepthStencilStateObject>(hd)->~DepthStencilStateObject();
}

// --- Pipeline states ---------------------------------------------------------------------------------------

std::shared_ptr<const vgd::Shader> shader_of(D3D12DDI_HSHADER h)
{
    return h.pDrvPrivate ? object<ShaderObject>(h)->shader : nullptr;
}

SIZE_T APIENTRY calc_private_pipeline_state_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_PIPELINE_STATE_0075*)
{
    return sizeof(PipelineStateObject);
}

HRESULT APIENTRY create_pipeline_state(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_PIPELINE_STATE_0075* a,
                                       D3D12DDI_HPIPELINESTATE hp, D3D12DDI_HRTPIPELINESTATE)
{
    auto* obj = new (hp.pDrvPrivate) PipelineStateObject;
    auto root = a->hRootSignature.pDrvPrivate ? object<RootSignatureObject>(a->hRootSignature)->rs : nullptr;
    HRESULT hr;
    if (a->hComputeShader.pDrvPrivate) {
        vgd::ComputePipelineDesc desc{root, shader_of(a->hComputeShader)};
        hr = vgd::PipelineState::create_compute(std::move(desc), &obj->pso);
    } else if (a->hMeshShader.pDrvPrivate || a->hAmplificationShader.pDrvPrivate) {
        hr = E_NOTIMPL;   // caps report no mesh shaders
    } else {
        vgd::GraphicsPipelineDesc desc;
        desc.root_signature = root;
        desc.vs = shader_of(a->hVertexShader);
        desc.hs = shader_of(a->hHullShader);
        desc.ds = shader_of(a->hDomainShader);
        desc.gs = shader_of(a->hGeometryShader);
        desc.ps = shader_of(a->hPixelShader);
        if (a->hBlendState.pDrvPrivate)
            desc.blend = object<BlendStateObject>(a->hBlendState)->desc;
        desc.sample_mask = a->SampleMask;
        if (a->hRasterizerState.pDrvPrivate)
            desc.rasterizer = object<RasterizerStateObject>(a->hRasterizerState)->desc;
        if (a->hDepthStencilState.pDrvPrivate)
            desc.depth_stencil = object<DepthStencilStateObject>(a->hDepthStencilState)->desc;
        if (a->hElementLayout.pDrvPrivate)
            desc.input_layout = object<ElementLayoutObject>(a->hElementLayout)->elements;
        desc.strip_cut = static_cast<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(a->IBStripCutValue);
        desc.topology = static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(a->PrimitiveTopologyType);
        desc.num_render_targets = a->NumRenderTargets;
        for (int i = 0; i < 8; i++)
            desc.rtv_formats[i] = a->RTVFormats[i];
        desc.dsv_format = a->DSVFormat;
        desc.sample_desc = a->SampleDesc;
        hr = vgd::PipelineState::create_graphics(std::move(desc), &obj->pso);
    }
    if (SUCCEEDED(hr))
        hr = obj->pso->compile(*Device::from(h)->core);   // D3D12 compiles at creation; draws must not stall
    if (FAILED(hr)) {
        VGD_WARN("CreatePipelineState failed: 0x%08lx", hr);
        obj->~PipelineStateObject();
        return E_OUTOFMEMORY;   // creation DDIs may only report E_OUTOFMEMORY (or device removed)
    }
    VGD_VERBOSE("CreatePipelineState: %s", obj->pso->is_compute() ? "compute" : "graphics");
    return S_OK;
}

void APIENTRY destroy_pipeline_state(D3D12DDI_HDEVICE, D3D12DDI_HPIPELINESTATE hp)
{
    object<PipelineStateObject>(hp)->~PipelineStateObject();
}

} // namespace

void fill_pipeline_ddi(D3D12DDI_DEVICE_FUNCS_CORE_0088& t)
{
    using vgd::ShaderStage;
    t.pfnCalcPrivateShaderSize = calc_private_shader_size;
    t.pfnCalcPrivateTessellationShaderSize = calc_private_shader_size;
    t.pfnCreateVertexShader = create_shader<ShaderStage::vertex>;
    t.pfnCreatePixelShader = create_shader<ShaderStage::pixel>;
    t.pfnCreateGeometryShader = create_shader<ShaderStage::geometry>;
    t.pfnCreateComputeShader = create_shader<ShaderStage::compute>;
    t.pfnCreateHullShader = create_shader<ShaderStage::hull>;
    t.pfnCreateDomainShader = create_shader<ShaderStage::domain>;
    t.pfnDestroyShader = destroy_shader;

    t.pfnCalcPrivateElementLayoutSize = calc_private_element_layout_size;
    t.pfnCreateElementLayout = create_element_layout;
    t.pfnDestroyElementLayout = destroy_element_layout;
    t.pfnCalcPrivateBlendStateSize = calc_private_blend_state_size;
    t.pfnCreateBlendState = create_blend_state;
    t.pfnDestroyBlendState = destroy_blend_state;
    t.pfnCalcPrivateRasterizerStateSize = calc_private_rasterizer_state_size;
    t.pfnCreateRasterizerState = create_rasterizer_state;
    t.pfnDestroyRasterizerState = destroy_rasterizer_state;
    t.pfnCalcPrivateDepthStencilStateSize = calc_private_depth_stencil_state_size;
    t.pfnCreateDepthStencilState = create_depth_stencil_state;
    t.pfnDestroyDepthStencilState = destroy_depth_stencil_state;

    t.pfnCalcPrivatePipelineStateSize = calc_private_pipeline_state_size;
    t.pfnCreatePipelineState = create_pipeline_state;
    t.pfnDestroyPipelineState = destroy_pipeline_state;
}

} // namespace vgd::umd
