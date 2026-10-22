// SPDX-License-Identifier: MIT
// Shaders, fixed-function state objects, input layouts and pipeline state objects. The DDI creates the pieces of a
// pipeline separately; each is kept in the API's form, and CreatePipelineState hands them to vkd3d-proton as one
// pipeline state stream.
#include "device.h"
#include "log.h"
#include "objects.h"
#include "shader_container.h"

#include <cstring>
#include <new>

namespace vgd::umd {

namespace {

// --- Shaders -----------------------------------------------------------------------------------------------

SIZE_T APIENTRY calc_private_shader_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_SHADER_0026*)
{
    return sizeof(ShaderObject);
}

ShaderSignatures standard_signatures(const D3D12DDIARG_STAGE_IO_SIGNATURES* s)
{
    ShaderSignatures out;
    if (s) {
        out.input = s->pInputSignature;
        out.input_count = s->NumInputSignatureEntries;
        out.output = s->pOutputSignature;
        out.output_count = s->NumOutputSignatureEntries;
    }
    return out;
}

void create(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_SHADER_0026& args, const ShaderSignatures& signatures,
            D3D12DDI_HSHADER hs)
{
    auto* obj = new (hs.pDrvPrivate) ShaderObject;
    if (FAILED(build_shader_container(args.pShaderCode, signatures, &obj->container))) {
        const UINT* c = args.pShaderCode;
        VGD_WARN("CreateShader: unrecognized code, first words %08x %08x %08x %08x", c[0], c[1], c[2], c[3]);
        Device::from(h)->set_error(E_OUTOFMEMORY);
    }
}

void APIENTRY create_shader(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_SHADER_0026* args, D3D12DDI_HSHADER hs)
{
    create(h, *args, standard_signatures(args->IOSignatures.Standard), hs);
}

void APIENTRY create_compute_shader(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_SHADER_0026* args,
                                    D3D12DDI_HSHADER hs)
{
    create(h, *args, {}, hs);
}

void APIENTRY create_tessellation_shader(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_SHADER_0026* args,
                                         D3D12DDI_HSHADER hs)
{
    ShaderSignatures s;
    if (const auto* t = args->IOSignatures.Tessellation) {
        s.input = t->pInputSignature;
        s.input_count = t->NumInputSignatureEntries;
        s.output = t->pOutputSignature;
        s.output_count = t->NumOutputSignatureEntries;
        s.patch_constant = t->pPatchConstantSignature;
        s.patch_constant_count = t->NumPatchConstantSignatureEntries;
    }
    create(h, *args, s, hs);
}

SIZE_T APIENTRY calc_private_geometry_shader_with_stream_output(
    D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT_0026*)
{
    return sizeof(ShaderObject);
}

// The DDI names stream-output components by output register; the API by the semantics of the output signature,
// which the container gives the same names (shader_container.h).
void APIENTRY create_geometry_shader_with_stream_output(
    D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT_0026* args, D3D12DDI_HSHADER hs)
{
    const ShaderSignatures s = standard_signatures(args->CreateShader.IOSignatures.Standard);
    create(h, args->CreateShader, s, hs);
    auto* obj = object<ShaderObject>(hs);
    obj->stream_output = true;
    obj->so_rasterized_stream = args->RasterizedStream;
    obj->so_strides.assign(args->BufferStridesInBytes, args->BufferStridesInBytes + args->NumStrides);
    for (UINT i = 0; i < args->NumEntries; i++) {
        const auto& d = args->pOutputStreamDecl[i];
        D3D12_SO_DECLARATION_ENTRY e{};
        e.Stream = d.Stream;
        e.OutputSlot = static_cast<BYTE>(d.OutputSlot);
        UINT lo = 0, count = 0;
        for (UINT c = 0; c < 4; c++)
            if (d.RegisterMask & (1u << c)) {
                if (!count)
                    lo = c;
                count++;
            }
        e.ComponentCount = static_cast<BYTE>(count);
        UINT first = 0;
        if (d.RegisterIndex == ~0u || !output_semantic(s.output, s.output_count, d.Stream, d.RegisterIndex,
                                                         d.RegisterMask, &e.SemanticName, &e.SemanticIndex, &first)) {
            e.SemanticName = nullptr;   // a gap in the buffer
            e.SemanticIndex = 0;
        } else {
            e.StartComponent = static_cast<BYTE>(lo - first);
        }
        obj->so_entries.push_back(e);
    }
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
        D3D12_INPUT_ELEMENT_DESC d{};
        // Vertex inputs are not packed: each starts at component 0 of its register.
        d.SemanticName = generic_semantic(0);
        d.SemanticIndex = generic_semantic_index(e.InputRegister, 0);
        d.Format = e.Format;
        d.InputSlot = e.InputSlot;
        d.AlignedByteOffset = e.AlignedByteOffset;
        d.InputSlotClass = e.InputSlotClass == D3D12DDI_INPUT_CLASSIFICIATION_PER_INSTANCE_DATA   // sic, WDK spelling
                               ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                               : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        d.InstanceDataStepRate = e.InstanceDataStepRate;
        obj->elements.push_back(d);
    }
}

void APIENTRY destroy_element_layout(D3D12DDI_HDEVICE, D3D12DDI_HELEMENTLAYOUT hl)
{
    object<ElementLayoutObject>(hl)->~ElementLayoutObject();
}

// --- Fixed-function states ---------------------------------------------------------------------------------
// The DDI structs carry extra driver fields (library references, ScissorEnable, per-face stencil enables), so they
// are converted field by field into the API structs.

SIZE_T APIENTRY calc_private_blend_state_size(D3D12DDI_HDEVICE, const D3D12DDI_BLEND_DESC_0010*)
{
    return sizeof(BlendStateObject);
}

void APIENTRY create_blend_state(D3D12DDI_HDEVICE, const D3D12DDI_BLEND_DESC_0010* d, D3D12DDI_HBLENDSTATE hb)
{
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

// A face whose stencil test the DDI disables passes and keeps its stencil, which the API spells with per-face
// operations and masks.
D3D12_DEPTH_STENCILOP_DESC1 stencil_face(const D3D12DDI_DEPTH_STENCILOP_DESC& s, bool enabled, UINT8 read,
                                         UINT8 write)
{
    if (!enabled)
        return {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS,
                read, 0};
    return {static_cast<D3D12_STENCIL_OP>(s.StencilFailOp), static_cast<D3D12_STENCIL_OP>(s.StencilDepthFailOp),
            static_cast<D3D12_STENCIL_OP>(s.StencilPassOp), static_cast<D3D12_COMPARISON_FUNC>(s.StencilFunc), read,
            write};
}

void APIENTRY create_depth_stencil_state(D3D12DDI_HDEVICE, const D3D12DDI_DEPTH_STENCIL_DESC_0025* d,
                                         D3D12DDI_HDEPTHSTENCILSTATE hd)
{
    D3D12_DEPTH_STENCIL_DESC2 ds{};
    ds.DepthEnable = d->DepthEnable;
    ds.DepthWriteMask = static_cast<D3D12_DEPTH_WRITE_MASK>(d->DepthWriteMask);
    ds.DepthFunc = static_cast<D3D12_COMPARISON_FUNC>(d->DepthFunc);
    ds.StencilEnable = d->StencilEnable;
    ds.FrontFace = stencil_face(d->FrontFace, d->FrontEnable, d->StencilReadMask, d->StencilWriteMask);
    ds.BackFace = stencil_face(d->BackFace, d->BackEnable, d->StencilReadMask, d->StencilWriteMask);
    ds.DepthBoundsTestEnable = d->DepthBoundsTestEnable;
    new (hd.pDrvPrivate) DepthStencilStateObject{ds};
}

void APIENTRY destroy_depth_stencil_state(D3D12DDI_HDEVICE, D3D12DDI_HDEPTHSTENCILSTATE hd)
{
    object<DepthStencilStateObject>(hd)->~DepthStencilStateObject();
}

// --- Pipeline states ---------------------------------------------------------------------------------------

// A pipeline state stream: subobjects packed back to back, each padded to a multiple of a pointer's size.
class Stream {
public:
    template <class T>
    void add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, const T& data)
    {
        struct alignas(void*) Subobject {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
            T data;
        } s{type, data};
        const size_t at = bytes_.size();
        bytes_.resize(at + sizeof(s));
        std::memcpy(bytes_.data() + at, &s, sizeof(s));
    }
    D3D12_PIPELINE_STATE_STREAM_DESC desc() { return {bytes_.size(), bytes_.data()}; }

private:
    std::vector<uint8_t> bytes_;
};

const ShaderObject* shader_of(D3D12DDI_HSHADER h)
{
    return h.pDrvPrivate ? object<ShaderObject>(h) : nullptr;
}

SIZE_T APIENTRY calc_private_pipeline_state_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_PIPELINE_STATE_0075*)
{
    return sizeof(PipelineStateObject);
}

static_assert(sizeof(D3D12DDI_VIEW_INSTANCING_DESC) == sizeof(D3D12_VIEW_INSTANCING_DESC));
static_assert(sizeof(D3D12DDI_VIEW_INSTANCE_LOCATION) == sizeof(D3D12_VIEW_INSTANCE_LOCATION));

HRESULT APIENTRY create_pipeline_state(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_PIPELINE_STATE_0075* a,
                                       D3D12DDI_HPIPELINESTATE hp, D3D12DDI_HRTPIPELINESTATE)
{
    Device* d = Device::from(h);
    auto* obj = new (hp.pDrvPrivate) PipelineStateObject;
    ID3D12RootSignature* root =
        a->hRootSignature.pDrvPrivate ? object<RootSignatureObject>(a->hRootSignature)->rs.get() : nullptr;
    HRESULT hr;
    if (const ShaderObject* cs = shader_of(a->hComputeShader)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root;
        desc.CS = cs->bytecode();
        desc.NodeMask = a->NodeMask;
        hr = d->d3d->CreateComputePipelineState(&desc, __uuidof(ID3D12PipelineState), obj->pso.put_void());
    } else if (a->hMeshShader.pDrvPrivate || a->hAmplificationShader.pDrvPrivate || !d->d3d2) {
        hr = E_NOTIMPL;   // caps report no mesh shaders; graphics pipelines need the stream form
    } else {
        Stream s;
        s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, root);
        const struct {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
            D3D12DDI_HSHADER shader;
        } stages[] = {{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, a->hVertexShader},
                      {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS, a->hHullShader},
                      {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS, a->hDomainShader},
                      {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS, a->hGeometryShader},
                      {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, a->hPixelShader}};
        for (const auto& st : stages)
            if (const ShaderObject* sh = shader_of(st.shader))
                s.add(st.type, sh->bytecode());
        if (const ShaderObject* gs = shader_of(a->hGeometryShader); gs && gs->stream_output) {
            D3D12_STREAM_OUTPUT_DESC so{gs->so_entries.data(), static_cast<UINT>(gs->so_entries.size()),
                                        gs->so_strides.data(), static_cast<UINT>(gs->so_strides.size()),
                                        gs->so_rasterized_stream};
            s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT, so);
        }
        if (a->hBlendState.pDrvPrivate)
            s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, object<BlendStateObject>(a->hBlendState)->desc);
        s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, a->SampleMask);
        if (a->hRasterizerState.pDrvPrivate)
            s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER,
                  object<RasterizerStateObject>(a->hRasterizerState)->desc);
        if (a->hDepthStencilState.pDrvPrivate)
            s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2,
                  object<DepthStencilStateObject>(a->hDepthStencilState)->desc);
        if (a->hElementLayout.pDrvPrivate) {
            const auto& e = object<ElementLayoutObject>(a->hElementLayout)->elements;
            s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT,
                  D3D12_INPUT_LAYOUT_DESC{e.data(), static_cast<UINT>(e.size())});
        }
        s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE,
              static_cast<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(a->IBStripCutValue));
        s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY,
              static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(a->PrimitiveTopologyType));
        D3D12_RT_FORMAT_ARRAY rtv{};
        rtv.NumRenderTargets = a->NumRenderTargets;
        for (int i = 0; i < 8; i++)
            rtv.RTFormats[i] = a->RTVFormats[i];
        s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, rtv);
        s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, a->DSVFormat);
        s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, a->SampleDesc);
        s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK, a->NodeMask);
        if (const auto& vi = a->ViewInstancingDesc; vi.ViewInstanceCount)
            s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING,
                  D3D12_VIEW_INSTANCING_DESC{vi.ViewInstanceCount,
                                             reinterpret_cast<const D3D12_VIEW_INSTANCE_LOCATION*>(vi.pViewInstanceLocations),
                                             static_cast<D3D12_VIEW_INSTANCING_FLAGS>(vi.Flags)});
        D3D12_PIPELINE_STATE_STREAM_DESC desc = s.desc();
        hr = d->d3d2->CreatePipelineState(&desc, __uuidof(ID3D12PipelineState), obj->pso.put_void());
    }
    if (FAILED(hr)) {
        VGD_WARN("CreatePipelineState (%s) failed: 0x%08lx", a->hComputeShader.pDrvPrivate ? "compute" : "graphics", hr);
        obj->~PipelineStateObject();
        return E_OUTOFMEMORY;   // creation DDIs may only report E_OUTOFMEMORY (or device removed)
    }
    return S_OK;
}

void APIENTRY destroy_pipeline_state(D3D12DDI_HDEVICE, D3D12DDI_HPIPELINESTATE hp)
{
    object<PipelineStateObject>(hp)->~PipelineStateObject();
}

// --- Pipeline libraries --------------------------------------------------------------------------------------
// ID3D12PipelineLibrary is the application's cache of compiled pipelines. The runtime keeps the names: a pipeline
// loaded from a library reaches CreatePipelineState with its whole description and a LibraryReference, so a library
// that holds nothing is correct, and loads cost a compile (Dawn creates one for its cache). Its serialized form is a
// header only, and any initial data is accepted: nothing in it is used.

struct PipelineLibraryObject {};

constexpr char kLibraryHeader[16] = "vgd3d12 pplib 1";

SIZE_T APIENTRY calc_private_pipeline_library_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_PIPELINE_LIBRARY_0010*)
{
    return sizeof(PipelineLibraryObject);
}

HRESULT APIENTRY create_pipeline_library(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_PIPELINE_LIBRARY_0010*,
                                         D3D12DDI_HPIPELINELIBRARY h)
{
    new (h.pDrvPrivate) PipelineLibraryObject;
    return S_OK;
}

void APIENTRY destroy_pipeline_library(D3D12DDI_HDEVICE, D3D12DDI_HPIPELINELIBRARY h)
{
    object<PipelineLibraryObject>(h)->~PipelineLibraryObject();
}

HRESULT APIENTRY add_pipeline_state_to_library(D3D12DDI_HDEVICE, D3D12DDI_HPIPELINELIBRARY, D3D12DDI_HPIPELINESTATE,
                                               UINT)
{
    return S_OK;
}

SIZE_T APIENTRY calc_serialized_library_size(D3D12DDI_HDEVICE, D3D12DDI_HPIPELINELIBRARY)
{
    return sizeof(kLibraryHeader);
}

HRESULT APIENTRY serialize_library(D3D12DDI_HDEVICE, D3D12DDI_HPIPELINELIBRARY, void* blob)
{
    std::memcpy(blob, kLibraryHeader, sizeof(kLibraryHeader));
    return S_OK;
}

} // namespace

void fill_pipeline_ddi(D3D12DDI_DEVICE_FUNCS_CORE_0088& t)
{
    t.pfnCalcPrivatePipelineLibrarySize = calc_private_pipeline_library_size;
    t.pfnCreatePipelineLibrary = create_pipeline_library;
    t.pfnDestroyPipelineLibrary = destroy_pipeline_library;
    t.pfnAddPipelineStateToLibrary = add_pipeline_state_to_library;
    t.pfnCalcSerializedLibrarySize = calc_serialized_library_size;
    t.pfnSerializeLibrary = serialize_library;

    t.pfnCalcPrivateShaderSize = calc_private_shader_size;
    t.pfnCalcPrivateTessellationShaderSize = calc_private_shader_size;
    t.pfnCreateVertexShader = create_shader;
    t.pfnCreatePixelShader = create_shader;
    t.pfnCreateGeometryShader = create_shader;
    t.pfnCreateComputeShader = create_compute_shader;
    t.pfnCreateHullShader = create_tessellation_shader;
    t.pfnCreateDomainShader = create_tessellation_shader;
    t.pfnCalcPrivateGeometryShaderWithStreamOutput = calc_private_geometry_shader_with_stream_output;
    t.pfnCreateGeometryShaderWithStreamOutput = create_geometry_shader_with_stream_output;
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
