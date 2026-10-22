// SPDX-License-Identifier: MIT
// Pipeline bindings, draws and dispatches: each DDI entry is the immediate context's method of the same name, with
// DDI handles turned into the DXVK objects behind them.
#include "device.h"

#include <array>

namespace vgd::umd {

namespace {

constexpr UINT kMaxSlots = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;   // the largest binding array, 128

template <class T, class H>
std::array<T*, kMaxSlots> views(const H* handles, UINT count)
{
    std::array<T*, kMaxSlots> out{};
    for (UINT i = 0; handles && i < count && i < kMaxSlots; i++)
        out[i] = view_of<T>(handles[i]);
    return out;
}

std::array<ID3D11SamplerState*, kMaxSlots> samplers(const D3D10DDI_HSAMPLER* handles, UINT count)
{
    std::array<ID3D11SamplerState*, kMaxSlots> out{};
    for (UINT i = 0; handles && i < count && i < kMaxSlots; i++)
        out[i] = state_of<ID3D11SamplerState>(handles[i]);
    return out;
}

std::array<ID3D11Buffer*, kMaxSlots> buffers(const D3D10DDI_HRESOURCE* handles, UINT count)
{
    std::array<ID3D11Buffer*, kMaxSlots> out{};
    for (UINT i = 0; handles && i < count && i < kMaxSlots; i++)
        out[i] = buffer_of(handles[i]);
    return out;
}

ID3D11DeviceContext4* ctx(D3D10DDI_HDEVICE h) { return Device::from(h)->ctx.get(); }

// --- Per-stage bindings ------------------------------------------------------------------------------------

#define VGD_STAGE(Ddi, Api, Shader)                                                                                 \
    void APIENTRY Ddi##_set_shader(D3D10DDI_HDEVICE h, D3D10DDI_HSHADER hs)                                         \
    {                                                                                                               \
        ctx(h)->Api##SetShader(shader_of<Shader>(hs), nullptr, 0);                                                  \
    }                                                                                                               \
    void APIENTRY Ddi##_set_shader_with_ifaces(D3D10DDI_HDEVICE h, D3D10DDI_HSHADER hs, UINT instances,            \
                                               const UINT*, const D3D11DDIARG_POINTERDATA*)                         \
    {                                                                                                               \
        if (instances)                                                                                              \
            VGD_WARN(#Api "SetShaderWithIfaces: %u class instances not supported", instances);                       \
        ctx(h)->Api##SetShader(shader_of<Shader>(hs), nullptr, 0);                                                  \
    }                                                                                                               \
    void APIENTRY Ddi##_set_srvs(D3D10DDI_HDEVICE h, UINT start, UINT count,                                        \
                                 const D3D10DDI_HSHADERRESOURCEVIEW* hv)                                            \
    {                                                                                                               \
        ctx(h)->Api##SetShaderResources(start, count, views<ID3D11ShaderResourceView>(hv, count).data());           \
    }                                                                                                               \
    void APIENTRY Ddi##_set_samplers(D3D10DDI_HDEVICE h, UINT start, UINT count, const D3D10DDI_HSAMPLER* hs)      \
    {                                                                                                               \
        ctx(h)->Api##SetSamplers(start, count, samplers(hs, count).data());                                         \
    }                                                                                                               \
    void APIENTRY Ddi##_set_constant_buffers(D3D10DDI_HDEVICE h, UINT start, UINT count,                            \
                                             const D3D10DDI_HRESOURCE* hb, const UINT* first, const UINT* num)      \
    {                                                                                                               \
        ctx(h)->Api##SetConstantBuffers1(start, count, buffers(hb, count).data(), first, num);                      \
    }

VGD_STAGE(vs, VS, ID3D11VertexShader)
VGD_STAGE(ps, PS, ID3D11PixelShader)
VGD_STAGE(gs, GS, ID3D11GeometryShader)
VGD_STAGE(hs, HS, ID3D11HullShader)
VGD_STAGE(ds, DS, ID3D11DomainShader)
VGD_STAGE(cs, CS, ID3D11ComputeShader)
#undef VGD_STAGE

void APIENTRY cs_set_uavs(D3D10DDI_HDEVICE h, UINT start, UINT count, const D3D11DDI_HUNORDEREDACCESSVIEW* hv,
                          const UINT* initial_counts)
{
    ctx(h)->CSSetUnorderedAccessViews(start, count, views<ID3D11UnorderedAccessView>(hv, count).data(),
                                      initial_counts);
}

// --- Input assembly ----------------------------------------------------------------------------------------

void APIENTRY ia_set_input_layout(D3D10DDI_HDEVICE h, D3D10DDI_HELEMENTLAYOUT hl)
{
    ctx(h)->IASetInputLayout(state_of<ID3D11InputLayout>(hl));
}

void APIENTRY ia_set_vertex_buffers(D3D10DDI_HDEVICE h, UINT start, UINT count, const D3D10DDI_HRESOURCE* hb,
                                   const UINT* strides, const UINT* offsets)
{
    ctx(h)->IASetVertexBuffers(start, count, buffers(hb, count).data(), strides, offsets);
}

void APIENTRY ia_set_index_buffer(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE hb, DXGI_FORMAT format, UINT offset)
{
    ctx(h)->IASetIndexBuffer(buffer_of(hb), format, offset);
}

void APIENTRY ia_set_topology(D3D10DDI_HDEVICE h, D3D10_DDI_PRIMITIVE_TOPOLOGY topology)
{
    ctx(h)->IASetPrimitiveTopology(static_cast<D3D11_PRIMITIVE_TOPOLOGY>(topology));   // same values
}

// --- Draws and dispatches ----------------------------------------------------------------------------------

void APIENTRY draw(D3D10DDI_HDEVICE h, UINT vertices, UINT start)
{
    ctx(h)->Draw(vertices, start);
}

void APIENTRY draw_indexed(D3D10DDI_HDEVICE h, UINT indices, UINT start, INT base)
{
    ctx(h)->DrawIndexed(indices, start, base);
}

void APIENTRY draw_instanced(D3D10DDI_HDEVICE h, UINT vertices, UINT instances, UINT start, UINT start_instance)
{
    ctx(h)->DrawInstanced(vertices, instances, start, start_instance);
}

void APIENTRY draw_indexed_instanced(D3D10DDI_HDEVICE h, UINT indices, UINT instances, UINT start, INT base,
                                     UINT start_instance)
{
    ctx(h)->DrawIndexedInstanced(indices, instances, start, base, start_instance);
}

void APIENTRY draw_auto(D3D10DDI_HDEVICE h)
{
    ctx(h)->DrawAuto();
}

void APIENTRY draw_instanced_indirect(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE hb, UINT offset)
{
    ctx(h)->DrawInstancedIndirect(buffer_of(hb), offset);
}

void APIENTRY draw_indexed_instanced_indirect(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE hb, UINT offset)
{
    ctx(h)->DrawIndexedInstancedIndirect(buffer_of(hb), offset);
}

void APIENTRY dispatch(D3D10DDI_HDEVICE h, UINT x, UINT y, UINT z)
{
    ctx(h)->Dispatch(x, y, z);
}

void APIENTRY dispatch_indirect(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE hb, UINT offset)
{
    ctx(h)->DispatchIndirect(buffer_of(hb), offset);
}

// --- Stream output, rasterizer and output merger -----------------------------------------------------------

void APIENTRY so_set_targets(D3D10DDI_HDEVICE h, UINT count, UINT clear, const D3D10DDI_HRESOURCE* hb,
                             const UINT* offsets)
{
    // The API's call sets all four slots: those past `count` are unbound.
    ctx(h)->SOSetTargets(count, buffers(hb, count).data(), offsets);
}

void APIENTRY set_viewports(D3D10DDI_HDEVICE h, UINT count, UINT clear, const D3D10_DDI_VIEWPORT* viewports)
{
    static_assert(sizeof(D3D10_DDI_VIEWPORT) == sizeof(D3D11_VIEWPORT));
    ctx(h)->RSSetViewports(count, reinterpret_cast<const D3D11_VIEWPORT*>(viewports));
}

void APIENTRY set_scissor_rects(D3D10DDI_HDEVICE h, UINT count, UINT clear, const D3D10_DDI_RECT* rects)
{
    ctx(h)->RSSetScissorRects(count, rects);
}

void APIENTRY set_rasterizer_state(D3D10DDI_HDEVICE h, D3D10DDI_HRASTERIZERSTATE hs)
{
    ctx(h)->RSSetState(state_of<ID3D11RasterizerState1>(hs));
}

void APIENTRY set_blend_state(D3D10DDI_HDEVICE h, D3D10DDI_HBLENDSTATE hs, const FLOAT factor[4], UINT sample_mask)
{
    ctx(h)->OMSetBlendState(state_of<ID3D11BlendState1>(hs), factor, sample_mask);
}

void APIENTRY set_depth_stencil_state(D3D10DDI_HDEVICE h, D3D10DDI_HDEPTHSTENCILSTATE hs, UINT stencil_ref)
{
    ctx(h)->OMSetDepthStencilState(state_of<ID3D11DepthStencilState>(hs), stencil_ref);
}

// The complete output-merger state: render targets in [0, NumRTVs), unordered access views in
// [UAVStartSlot, UAVStartSlot + NumUAVs); the clear counts and the UAV range only say what changed.
void APIENTRY set_render_targets(D3D10DDI_HDEVICE h, const D3D10DDI_HRENDERTARGETVIEW* hrtv, UINT num_rtvs,
                                 UINT clear, D3D10DDI_HDEPTHSTENCILVIEW hdsv, const D3D11DDI_HUNORDEREDACCESSVIEW* huav,
                                 const UINT* initial_counts, UINT uav_start, UINT num_uavs, UINT, UINT)
{
    Device* d = Device::from(h);
    auto rtvs = views<ID3D11RenderTargetView>(hrtv, num_rtvs);
    ID3D11DepthStencilView* dsv = view_of<ID3D11DepthStencilView>(hdsv);
    d->om_rtvs.assign(rtvs.begin(), rtvs.begin() + std::min<UINT>(num_rtvs, kMaxSlots));
    d->om_dsv = dsv;
    d->ctx->OMSetRenderTargetsAndUnorderedAccessViews(num_rtvs, rtvs.data(), dsv, num_uavs ? uav_start : num_rtvs,
                                                      num_uavs, views<ID3D11UnorderedAccessView>(huav, num_uavs).data(),
                                                      initial_counts);
}

// DXVK tracks hazards itself.
void APIENTRY srv_read_after_write_hazard(D3D10DDI_HDEVICE, D3D10DDI_HSHADERRESOURCEVIEW, D3D10DDI_HRESOURCE) {}

} // namespace

void fill_draw_ddi(D3D11_1DDI_DEVICEFUNCS& t)
{
#define VGD_FILL_STAGE(Ddi, Name)                                                                                   \
    t.pfn##Name##SetShader = Ddi##_set_shader;                                                                      \
    t.pfn##Name##SetShaderWithIfaces = Ddi##_set_shader_with_ifaces;                                                \
    t.pfn##Name##SetShaderResources = Ddi##_set_srvs;                                                               \
    t.pfn##Name##SetSamplers = Ddi##_set_samplers;                                                                  \
    t.pfn##Name##SetConstantBuffers = Ddi##_set_constant_buffers;
    VGD_FILL_STAGE(vs, Vs)
    VGD_FILL_STAGE(ps, Ps)
    VGD_FILL_STAGE(gs, Gs)
    VGD_FILL_STAGE(hs, Hs)
    VGD_FILL_STAGE(ds, Ds)
    VGD_FILL_STAGE(cs, Cs)
#undef VGD_FILL_STAGE
    t.pfnCsSetUnorderedAccessViews = cs_set_uavs;
    t.pfnIaSetInputLayout = ia_set_input_layout;
    t.pfnIaSetVertexBuffers = ia_set_vertex_buffers;
    t.pfnIaSetIndexBuffer = ia_set_index_buffer;
    t.pfnIaSetTopology = ia_set_topology;
    t.pfnDraw = draw;
    t.pfnDrawIndexed = draw_indexed;
    t.pfnDrawInstanced = draw_instanced;
    t.pfnDrawIndexedInstanced = draw_indexed_instanced;
    t.pfnDrawAuto = draw_auto;
    t.pfnDrawInstancedIndirect = draw_instanced_indirect;
    t.pfnDrawIndexedInstancedIndirect = draw_indexed_instanced_indirect;
    t.pfnDispatch = dispatch;
    t.pfnDispatchIndirect = dispatch_indirect;
    t.pfnSoSetTargets = so_set_targets;
    t.pfnSetViewports = set_viewports;
    t.pfnSetScissorRects = set_scissor_rects;
    t.pfnSetRasterizerState = set_rasterizer_state;
    t.pfnSetBlendState = set_blend_state;
    t.pfnSetDepthStencilState = set_depth_stencil_state;
    t.pfnSetRenderTargets = set_render_targets;
    t.pfnShaderResourceViewReadAfterWriteHazard = srv_read_after_write_hazard;
}

} // namespace vgd::umd
