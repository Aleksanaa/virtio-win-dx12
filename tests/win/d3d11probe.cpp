// SPDX-License-Identifier: MIT
// Guest probe for the D3D11 UMD: creates a D3D11 device on each hardware adapter through the system runtime and runs
// every test on it, printing PASS/FAIL per test. Shaders are compiled in the guest by d3dcompiler_47.dll.
//   --present  also show swapchains in a window for a few seconds each (watch them): flip model, then the legacy
//              blt model, then ResizeBuffers.
#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

DWORD g_main_thread = 0;

// Reports where an access violation happened (module + offset, address accessed, thread) before the process dies.
LONG CALLBACK report_crash(EXCEPTION_POINTERS* e)
{
    const EXCEPTION_RECORD* r = e->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(r->ExceptionAddress), &mod))
        GetModuleFileNameA(mod, name, sizeof(name));
    std::printf("ACCESS VIOLATION in %s+0x%llx, %s address 0x%llx, thread %lu%s\n", name,
                static_cast<unsigned long long>(static_cast<char*>(r->ExceptionAddress) - reinterpret_cast<char*>(mod)),
                r->ExceptionInformation[0] == 1 ? "writing" : "reading",
                static_cast<unsigned long long>(r->ExceptionInformation[1]), GetCurrentThreadId(),
                GetCurrentThreadId() == g_main_thread ? " (main)" : "");
    return EXCEPTION_CONTINUE_SEARCH;
}

template <class T>
struct Ref {
    T* p = nullptr;
    Ref() = default;
    Ref(const Ref&) = delete;
    ~Ref()
    {
        if (p)
            p->Release();
    }
    T** put() { return &p; }
    T* operator->() const { return p; }
    operator T*() const { return p; }
};

using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                      UINT, UINT, ID3DBlob**, ID3DBlob**);
D3DCompileFn g_compile = nullptr;

bool compile(const char* source, const char* entry, const char* target, Ref<ID3DBlob>* out)
{
    Ref<ID3DBlob> errors;
    HRESULT hr = g_compile(source, std::strlen(source), entry, nullptr, nullptr, entry, target, 0, 0, out->put(),
                           errors.put());
    if (FAILED(hr)) {
        std::printf("    compile %s (%s) failed: 0x%08lx %s\n", entry, target, hr,
                    errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        return false;
    }
    return true;
}

bool check(const char* test, const char* what, HRESULT hr)
{
    if (FAILED(hr))
        std::printf("    %s: %s failed: 0x%08lx\n", test, what, hr);
    return SUCCEEDED(hr);
}

bool result(const char* test, bool ok)
{
    std::printf("  %s: %s\n", test, ok ? "PASS" : "FAIL");
    return ok;
}

// Reads `tex` (a 2D texture, subresource 0) back through a staging copy as 32-bit texels.
bool read_back(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, std::vector<uint32_t>* texels,
               UINT* width)
{
    D3D11_TEXTURE2D_DESC desc;
    tex->GetDesc(&desc);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    desc.SampleDesc = {1, 0};
    Ref<ID3D11Texture2D> staging;
    if (!check("readback", "CreateTexture2D(staging)", dev->CreateTexture2D(&desc, nullptr, staging.put())))
        return false;
    ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE m;
    if (!check("readback", "Map", ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m)))
        return false;
    texels->resize(desc.Width * desc.Height);
    for (UINT y = 0; y < desc.Height; y++)
        std::memcpy(texels->data() + y * desc.Width, static_cast<uint8_t*>(m.pData) + y * m.RowPitch, desc.Width * 4);
    ctx->Unmap(staging, 0);
    *width = desc.Width;
    return true;
}

bool expect_texel(const char* test, const std::vector<uint32_t>& t, UINT width, UINT x, UINT y, uint32_t want)
{
    const uint32_t got = t[y * width + x];
    if (got != want)
        std::printf("    %s: texel (%u, %u) is 0x%08x, expected 0x%08x\n", test, x, y, got, want);
    return got == want;
}

Ref<ID3D11Texture2D>* make_target(ID3D11Device* dev, UINT size, UINT samples, Ref<ID3D11Texture2D>* tex,
                                  Ref<ID3D11RenderTargetView>* rtv)
{
    D3D11_TEXTURE2D_DESC d{size, size, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {samples, 0}, D3D11_USAGE_DEFAULT,
                           D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    if (FAILED(dev->CreateTexture2D(&d, nullptr, tex->put())) ||
        FAILED(dev->CreateRenderTargetView(*tex, nullptr, rtv->put())))
        return nullptr;
    return tex;
}

void set_viewport(ID3D11DeviceContext* ctx, UINT size)
{
    D3D11_VIEWPORT vp{0, 0, float(size), float(size), 0, 1};
    ctx->RSSetViewports(1, &vp);
}

// --- Tests -----------------------------------------------------------------------------------------------------

// A clear, read back through a staging copy.
bool clear_test(ID3D11Device* dev, ID3D11DeviceContext* ctx)
{
    const char* t = "clear";
    Ref<ID3D11Texture2D> tex;
    Ref<ID3D11RenderTargetView> rtv;
    if (!make_target(dev, 64, 1, &tex, &rtv))
        return result(t, false);
    const float color[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    ctx->ClearRenderTargetView(rtv, color);
    std::vector<uint32_t> texels;
    UINT w;
    bool ok = read_back(dev, ctx, tex, &texels, &w) && expect_texel(t, texels, w, 0, 0, 0xff0000ff) &&
              expect_texel(t, texels, w, 63, 63, 0xff0000ff);
    return result(t, ok);
}

const char* kTriangleHlsl = R"(
cbuffer Tint : register(b1) { float4 tint; };
struct V { float2 pos : POSITION; float4 color : COLOR; };
struct P { float4 pos : SV_Position; float4 color : COLOR; };
P vs(V v) { P p; p.pos = float4(v.pos, 0, 1); p.color = v.color; return p; }
float4 ps(P p) : SV_Target { return p.color * tint; }
)";

// A triangle from a vertex buffer through an input layout, a constant buffer at slot 1, the left half only.
bool triangle_test(ID3D11Device* dev, ID3D11DeviceContext* ctx)
{
    const char* t = "triangle";
    Ref<ID3DBlob> vsb, psb;
    if (!compile(kTriangleHlsl, "vs", "vs_4_0", &vsb) || !compile(kTriangleHlsl, "ps", "ps_4_0", &psb))
        return result(t, false);
    Ref<ID3D11VertexShader> vs;
    Ref<ID3D11PixelShader> ps;
    Ref<ID3D11InputLayout> layout;
    const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    // Covers x < 0 (the left half) entirely.
    const float verts[] = {-1, -3, 0, 1, 0, 1, -1, 3, 0, 1, 0, 1, 0, 0, 0, 1, 0, 1};
    const float tint[4] = {1, 1, 1, 1};
    Ref<ID3D11Buffer> vb, cb;
    D3D11_BUFFER_DESC vbd{sizeof(verts), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA vbi{verts, 0, 0};
    D3D11_BUFFER_DESC cbd{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA cbi{tint, 0, 0};
    Ref<ID3D11Texture2D> tex;
    Ref<ID3D11RenderTargetView> rtv;
    if (!check(t, "CreateVertexShader", dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, vs.put())) ||
        !check(t, "CreatePixelShader", dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, ps.put())) ||
        !check(t, "CreateInputLayout", dev->CreateInputLayout(elements, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), layout.put())) ||
        !check(t, "CreateBuffer(vertices)", dev->CreateBuffer(&vbd, &vbi, vb.put())) ||
        !check(t, "CreateBuffer(constants)", dev->CreateBuffer(&cbd, &cbi, cb.put())) ||
        !make_target(dev, 64, 1, &tex, &rtv))
        return result(t, false);
    const float black[4] = {0, 0, 0, 1};
    ctx->ClearRenderTargetView(rtv, black);
    ID3D11RenderTargetView* rtvs[] = {rtv};
    ctx->OMSetRenderTargets(1, rtvs, nullptr);
    set_viewport(ctx, 64);
    UINT stride = 24, offset = 0;
    ID3D11Buffer* vbs[] = {vb};
    ctx->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
    ctx->IASetInputLayout(layout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ID3D11Buffer* cbs[] = {cb};
    ctx->PSSetConstantBuffers(1, 1, cbs);
    ctx->Draw(3, 0);
    std::vector<uint32_t> texels;
    UINT w;
    bool ok = read_back(dev, ctx, tex, &texels, &w) && expect_texel(t, texels, w, 8, 32, 0xff00ff00) &&
              expect_texel(t, texels, w, 56, 32, 0xff000000);
    ctx->ClearState();
    return result(t, ok);
}

const char* kTexturedHlsl = R"(
Texture2D tex : register(t2);
SamplerState smp : register(s1);
struct P { float4 pos : SV_Position; float2 uv : TEXCOORD; };
P vs(uint id : SV_VertexID) {
    P p; p.uv = float2((id << 1) & 2, id & 2); p.pos = float4(p.uv * float2(2, -2) + float2(-1, 1), 0, 1); return p;
}
float4 ps(P p) : SV_Target { return tex.Sample(smp, p.uv); }
)";

// A 2x2 texture with initial data sampled (point filtering) over the whole target: four quadrants.
bool textured_test(ID3D11Device* dev, ID3D11DeviceContext* ctx)
{
    const char* t = "textured";
    Ref<ID3DBlob> vsb, psb;
    if (!compile(kTexturedHlsl, "vs", "vs_4_0", &vsb) || !compile(kTexturedHlsl, "ps", "ps_4_0", &psb))
        return result(t, false);
    Ref<ID3D11VertexShader> vs;
    Ref<ID3D11PixelShader> ps;
    const uint32_t texels_in[4] = {0xff0000ff, 0xff00ff00, 0xffff0000, 0xffffffff};
    D3D11_TEXTURE2D_DESC td{2, 2, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_IMMUTABLE,
                            D3D11_BIND_SHADER_RESOURCE, 0, 0};
    D3D11_SUBRESOURCE_DATA ti{texels_in, 8, 0};
    Ref<ID3D11Texture2D> src, tex;
    Ref<ID3D11ShaderResourceView> srv;
    Ref<ID3D11SamplerState> smp;
    Ref<ID3D11RenderTargetView> rtv;
    D3D11_SAMPLER_DESC sd{D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                          D3D11_TEXTURE_ADDRESS_CLAMP, 0, 1, D3D11_COMPARISON_NEVER, {}, 0, D3D11_FLOAT32_MAX};
    if (!check(t, "CreateVertexShader", dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, vs.put())) ||
        !check(t, "CreatePixelShader", dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, ps.put())) ||
        !check(t, "CreateTexture2D", dev->CreateTexture2D(&td, &ti, src.put())) ||
        !check(t, "CreateShaderResourceView", dev->CreateShaderResourceView(src, nullptr, srv.put())) ||
        !check(t, "CreateSamplerState", dev->CreateSamplerState(&sd, smp.put())) || !make_target(dev, 64, 1, &tex, &rtv))
        return result(t, false);
    ID3D11RenderTargetView* rtvs[] = {rtv};
    ctx->OMSetRenderTargets(1, rtvs, nullptr);
    set_viewport(ctx, 64);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = {srv};
    ctx->PSSetShaderResources(2, 1, srvs);
    ID3D11SamplerState* smps[] = {smp};
    ctx->PSSetSamplers(1, 1, smps);
    ctx->Draw(3, 0);
    std::vector<uint32_t> texels;
    UINT w;
    bool ok = read_back(dev, ctx, tex, &texels, &w) && expect_texel(t, texels, w, 8, 8, 0xff0000ff) &&
              expect_texel(t, texels, w, 56, 8, 0xff00ff00) && expect_texel(t, texels, w, 8, 56, 0xffff0000) &&
              expect_texel(t, texels, w, 56, 56, 0xffffffff);
    ctx->ClearState();
    return result(t, ok);
}

const char* kDepthHlsl = R"(
cbuffer C : register(b0) { float depth; float3 pad; float4 color; };
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), depth, 1);
}
float4 ps() : SV_Target { return color; }
)";

// Two full-screen draws with a less-than depth test (the far one, drawn second, loses) into a 4x MSAA target with a
// depth buffer, resolved.
bool depth_test(ID3D11Device* dev, ID3D11DeviceContext* ctx)
{
    const char* t = "depth+msaa";
    Ref<ID3DBlob> vsb, psb;
    if (!compile(kDepthHlsl, "vs", "vs_4_0", &vsb) || !compile(kDepthHlsl, "ps", "ps_4_0", &psb))
        return result(t, false);
    Ref<ID3D11VertexShader> vs;
    Ref<ID3D11PixelShader> ps;
    Ref<ID3D11Texture2D> ms, resolved, depth;
    Ref<ID3D11RenderTargetView> rtv, unused;
    Ref<ID3D11DepthStencilView> dsv;
    Ref<ID3D11DepthStencilState> dss;
    Ref<ID3D11Buffer> cb;
    UINT levels = 0;
    dev->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, 4, &levels);
    D3D11_TEXTURE2D_DESC dd{64, 64, 1, 1, DXGI_FORMAT_D32_FLOAT, {4, 0}, D3D11_USAGE_DEFAULT,
                            D3D11_BIND_DEPTH_STENCIL, 0, 0};
    D3D11_DEPTH_STENCIL_DESC dsd{TRUE, D3D11_DEPTH_WRITE_MASK_ALL, D3D11_COMPARISON_LESS, FALSE, 0xff, 0xff, {}, {}};
    D3D11_BUFFER_DESC cbd{32, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    if (levels == 0 || !make_target(dev, 64, 4, &ms, &rtv) || !make_target(dev, 64, 1, &resolved, &unused) ||
        !check(t, "CreateVertexShader", dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, vs.put())) ||
        !check(t, "CreatePixelShader", dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, ps.put())) ||
        !check(t, "CreateTexture2D(depth)", dev->CreateTexture2D(&dd, nullptr, depth.put())) ||
        !check(t, "CreateDepthStencilView", dev->CreateDepthStencilView(depth, nullptr, dsv.put())) ||
        !check(t, "CreateDepthStencilState", dev->CreateDepthStencilState(&dsd, dss.put())) ||
        !check(t, "CreateBuffer", dev->CreateBuffer(&cbd, nullptr, cb.put()))) {
        std::printf("    4x MSAA quality levels: %u\n", levels);
        return result(t, false);
    }
    const float black[4] = {0, 0, 0, 1};
    ctx->ClearRenderTargetView(rtv, black);
    ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    ID3D11RenderTargetView* rtvs[] = {rtv};
    ctx->OMSetRenderTargets(1, rtvs, dsv);
    ctx->OMSetDepthStencilState(dss, 0);
    set_viewport(ctx, 64);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ID3D11Buffer* cbs[] = {cb};
    ctx->VSSetConstantBuffers(0, 1, cbs);
    ctx->PSSetConstantBuffers(0, 1, cbs);
    const float draws[2][8] = {{0.25f, 0, 0, 0, 0, 0, 1, 1}, {0.75f, 0, 0, 0, 1, 0, 0, 1}};   // near blue, far red
    for (const auto& c : draws) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (!check(t, "Map(WRITE_DISCARD)", ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
            return result(t, false);
        std::memcpy(m.pData, c, sizeof(c));
        ctx->Unmap(cb, 0);
        ctx->Draw(3, 0);
    }
    ctx->ResolveSubresource(resolved, 0, ms, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    std::vector<uint32_t> texels;
    UINT w;
    bool ok = read_back(dev, ctx, resolved, &texels, &w) && expect_texel(t, texels, w, 32, 32, 0xffff0000);
    ctx->ClearState();
    return result(t, ok);
}

const char* kComputeHlsl = R"(
RWBuffer<uint> outv : register(u1);
[numthreads(64, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) { outv[id.x] = id.x * 3 + 1; }
)";

// A typed UAV buffer cleared, then written by a dispatch of 4 groups of 64.
bool compute_test(ID3D11Device* dev, ID3D11DeviceContext* ctx)
{
    const char* t = "compute";
    Ref<ID3DBlob> csb;
    if (!compile(kComputeHlsl, "cs", "cs_5_0", &csb))
        return result(t, false);
    constexpr UINT n = 256;
    Ref<ID3D11ComputeShader> cs;
    Ref<ID3D11Buffer> buf, staging;
    Ref<ID3D11UnorderedAccessView> uav;
    D3D11_BUFFER_DESC bd{n * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, 0, 0};
    D3D11_BUFFER_DESC sd{n * 4, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R32_UINT;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer = {0, n, 0};
    if (!check(t, "CreateComputeShader", dev->CreateComputeShader(csb->GetBufferPointer(), csb->GetBufferSize(), nullptr, cs.put())) ||
        !check(t, "CreateBuffer", dev->CreateBuffer(&bd, nullptr, buf.put())) ||
        !check(t, "CreateBuffer(staging)", dev->CreateBuffer(&sd, nullptr, staging.put())) ||
        !check(t, "CreateUnorderedAccessView", dev->CreateUnorderedAccessView(buf, &ud, uav.put())))
        return result(t, false);
    const UINT zero[4] = {7, 7, 7, 7};
    ctx->ClearUnorderedAccessViewUint(uav, zero);
    ctx->CSSetShader(cs, nullptr, 0);
    ID3D11UnorderedAccessView* uavs[] = {uav};
    ctx->CSSetUnorderedAccessViews(1, 1, uavs, nullptr);
    ctx->Dispatch(n / 64, 1, 1);
    ctx->CopyResource(staging, buf);
    D3D11_MAPPED_SUBRESOURCE m;
    bool ok = check(t, "Map", ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m));
    if (ok) {
        const auto* v = static_cast<const uint32_t*>(m.pData);
        for (UINT i = 0; i < n && ok; i++)
            if (v[i] != i * 3 + 1) {
                std::printf("    %s: value %u is %u, expected %u\n", t, i, v[i], i * 3 + 1);
                ok = false;
            }
        ctx->Unmap(staging, 0);
    }
    ctx->ClearState();
    return result(t, ok);
}

// An event, an occlusion query around a full-screen draw of 64x64 pixels, and timestamps.
bool queries_test(ID3D11Device* dev, ID3D11DeviceContext* ctx)
{
    const char* t = "queries";
    Ref<ID3DBlob> vsb, psb;
    if (!compile(kTexturedHlsl, "vs", "vs_4_0", &vsb) || !compile(kDepthHlsl, "ps", "ps_4_0", &psb))
        return result(t, false);
    Ref<ID3D11VertexShader> vs;
    Ref<ID3D11PixelShader> ps;
    Ref<ID3D11Texture2D> tex;
    Ref<ID3D11RenderTargetView> rtv;
    Ref<ID3D11Query> event, occlusion, disjoint, ts0, ts1;
    D3D11_QUERY_DESC qd[] = {{D3D11_QUERY_EVENT, 0}, {D3D11_QUERY_OCCLUSION, 0}, {D3D11_QUERY_TIMESTAMP_DISJOINT, 0},
                             {D3D11_QUERY_TIMESTAMP, 0}};
    if (!check(t, "CreateVertexShader", dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, vs.put())) ||
        !check(t, "CreatePixelShader", dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, ps.put())) ||
        !make_target(dev, 64, 1, &tex, &rtv) || !check(t, "CreateQuery(event)", dev->CreateQuery(&qd[0], event.put())) ||
        !check(t, "CreateQuery(occlusion)", dev->CreateQuery(&qd[1], occlusion.put())) ||
        !check(t, "CreateQuery(disjoint)", dev->CreateQuery(&qd[2], disjoint.put())) ||
        !check(t, "CreateQuery(timestamp)", dev->CreateQuery(&qd[3], ts0.put())) ||
        !check(t, "CreateQuery(timestamp)", dev->CreateQuery(&qd[3], ts1.put())))
        return result(t, false);
    ID3D11RenderTargetView* rtvs[] = {rtv};
    ctx->OMSetRenderTargets(1, rtvs, nullptr);
    set_viewport(ctx, 64);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->Begin(disjoint);
    ctx->End(ts0);
    ctx->Begin(occlusion);
    ctx->Draw(3, 0);
    ctx->End(occlusion);
    ctx->End(ts1);
    ctx->End(disjoint);
    ctx->End(event);
    auto wait = [&](ID3D11Query* q, void* data, UINT size) {
        for (int i = 0; i < 5000; i++) {
            HRESULT hr = ctx->GetData(q, data, size, 0);
            if (hr == S_OK)
                return true;
            if (FAILED(hr)) {
                std::printf("    %s: GetData failed: 0x%08lx\n", t, hr);
                return false;
            }
            Sleep(1);
        }
        std::printf("    %s: GetData timed out\n", t);
        return false;
    };
    BOOL done = FALSE;
    UINT64 samples = 0, t0 = 0, t1 = 0;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    bool ok = wait(event, &done, sizeof(done)) && done && wait(occlusion, &samples, sizeof(samples)) &&
              wait(disjoint, &dj, sizeof(dj)) && wait(ts0, &t0, sizeof(t0)) && wait(ts1, &t1, sizeof(t1));
    std::printf("    %s: %llu samples passed, timestamps %llu..%llu at %llu Hz%s\n", t,
                static_cast<unsigned long long>(samples), static_cast<unsigned long long>(t0),
                static_cast<unsigned long long>(t1), static_cast<unsigned long long>(dj.Frequency),
                dj.Disjoint ? " (disjoint)" : "");
    ok = ok && samples == 64 * 64 && t1 >= t0 && dj.Frequency;
    ctx->ClearState();
    return result(t, ok);
}

// Dynamic buffer mapping with discard and no-overwrite, and UpdateSubresource into part of a texture.
bool update_test(ID3D11Device* dev, ID3D11DeviceContext* ctx)
{
    const char* t = "map+update";
    Ref<ID3D11Buffer> dyn, staging;
    D3D11_BUFFER_DESC bd{256, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    D3D11_BUFFER_DESC sd{256, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
    if (!check(t, "CreateBuffer(dynamic)", dev->CreateBuffer(&bd, nullptr, dyn.put())) ||
        !check(t, "CreateBuffer(staging)", dev->CreateBuffer(&sd, nullptr, staging.put())))
        return result(t, false);
    D3D11_MAPPED_SUBRESOURCE m;
    bool ok = check(t, "Map(WRITE_DISCARD)", ctx->Map(dyn, 0, D3D11_MAP_WRITE_DISCARD, 0, &m));
    if (ok) {
        std::memset(m.pData, 0x11, 256);
        ctx->Unmap(dyn, 0);
    }
    ok = ok && check(t, "Map(WRITE_NO_OVERWRITE)", ctx->Map(dyn, 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m));
    if (ok) {
        std::memset(static_cast<uint8_t*>(m.pData) + 128, 0x22, 128);
        ctx->Unmap(dyn, 0);
    }
    ctx->CopyResource(staging, dyn);
    ok = ok && check(t, "Map(READ)", ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m));
    if (ok) {
        const auto* b = static_cast<const uint8_t*>(m.pData);
        if (b[0] != 0x11 || b[127] != 0x11 || b[128] != 0x22 || b[255] != 0x22) {
            std::printf("    %s: buffer bytes %02x %02x %02x %02x\n", t, b[0], b[127], b[128], b[255]);
            ok = false;
        }
        ctx->Unmap(staging, 0);
    }
    Ref<ID3D11Texture2D> tex;
    Ref<ID3D11RenderTargetView> rtv;
    ok = ok && make_target(dev, 64, 1, &tex, &rtv);
    if (ok) {
        const float black[4] = {0, 0, 0, 0};
        ctx->ClearRenderTargetView(rtv, black);
        std::vector<uint32_t> block(16 * 16, 0xff123456);
        const D3D11_BOX box{16, 16, 0, 32, 32, 1};
        ctx->UpdateSubresource(tex, 0, &box, block.data(), 16 * 4, 0);
        std::vector<uint32_t> texels;
        UINT w;
        ok = read_back(dev, ctx, tex, &texels, &w) && expect_texel(t, texels, w, 20, 20, 0xff123456) &&
             expect_texel(t, texels, w, 40, 40, 0);
    }
    return result(t, ok);
}

// A texture created shared on one device and opened on another: the path DWM takes to composite swapchains.
bool shared_test(ID3D11Device* dev, ID3D11DeviceContext* ctx, IDXGIAdapter* adapter)
{
    const char* t = "shared";
    Ref<ID3D11Device> dev2;
    Ref<ID3D11DeviceContext> ctx2;
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                   dev2.put(), nullptr, ctx2.put());
    if (!check(t, "D3D11CreateDevice(second)", hr))
        return result(t, false);
    D3D11_TEXTURE2D_DESC d{96, 64, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                           D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_SHARED};
    Ref<ID3D11Texture2D> tex;
    Ref<ID3D11RenderTargetView> rtv;
    Ref<IDXGIResource> dxgi;
    HANDLE shared = nullptr;
    if (!check(t, "CreateTexture2D(shared)", dev->CreateTexture2D(&d, nullptr, tex.put())) ||
        !check(t, "CreateRenderTargetView", dev->CreateRenderTargetView(tex, nullptr, rtv.put())) ||
        !check(t, "QueryInterface(IDXGIResource)", tex->QueryInterface(IID_PPV_ARGS(dxgi.put()))) ||
        !check(t, "GetSharedHandle", dxgi->GetSharedHandle(&shared)))
        return result(t, false);
    const float color[4] = {0.0f, 0.5f, 1.0f, 1.0f};
    ctx->ClearRenderTargetView(rtv, color);
    ctx->Flush();
    Ref<ID3D11Texture2D> opened;
    if (!check(t, "OpenSharedResource", dev2->OpenSharedResource(shared, IID_PPV_ARGS(opened.put()))))
        return result(t, false);
    std::vector<uint32_t> texels;
    UINT w;
    // B8G8R8A8: blue 0xff, green 0x80 (0.5 rounded), red 0.
    bool ok = read_back(dev2, ctx2, opened, &texels, &w) && expect_texel(t, texels, w, 0, 0, 0xffff8000) &&
              expect_texel(t, texels, w, 95, 63, 0xffff8000);
    return result(t, ok);
}

void pump_messages()
{
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

// Swapchains in a window (watch them): flip model, the legacy blt model, then flip model again after
// ResizeBuffers. Each phase cycles colours for about two seconds.
bool present_test(ID3D11Device* dev, ID3D11DeviceContext* ctx, IDXGIAdapter* adapter)
{
    const char* t = "present";
    Ref<IDXGIFactory2> factory;
    if (!check(t, "GetParent(factory)", adapter->GetParent(IID_PPV_ARGS(factory.put()))))
        return result(t, false);
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"d3d11probe";
    RegisterClassW(&wc);
    HWND wnd = CreateWindowW(L"d3d11probe", L"d3d11probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 640, 400,
                             nullptr, nullptr, wc.hInstance, nullptr);
    struct Phase {
        const char* name;
        DXGI_SWAP_EFFECT effect;
        UINT buffers, width, height;
    };
    const Phase phases[] = {{"flip discard 480x270", DXGI_SWAP_EFFECT_FLIP_DISCARD, 2, 480, 270},
                            {"blt discard 480x270", DXGI_SWAP_EFFECT_DISCARD, 1, 480, 270},
                            {"flip sequential, ResizeBuffers 640x360", DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, 2, 320, 180}};
    bool ok = true;
    for (const Phase& p : phases) {
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = p.width;
        sd.Height = p.height;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc = {1, 0};
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = p.buffers;
        sd.SwapEffect = p.effect;
        Ref<IDXGISwapChain1> sc;
        HRESULT hr = factory->CreateSwapChainForHwnd(dev, wnd, &sd, nullptr, nullptr, sc.put());
        if (!check(t, p.name, hr)) {
            ok = false;
            continue;
        }
        if (p.effect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL) {
            hr = sc->ResizeBuffers(0, 640, 360, DXGI_FORMAT_UNKNOWN, 0);
            if (!check(t, "ResizeBuffers", hr)) {
                ok = false;
                continue;
            }
        }
        UINT presented = 0;
        for (UINT frame = 0; frame < 120; frame++) {
            Ref<ID3D11Texture2D> back;
            Ref<ID3D11RenderTargetView> rtv;
            if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(back.put()))) ||
                FAILED(dev->CreateRenderTargetView(back, nullptr, rtv.put())))
                break;
            const float f = frame / 120.0f;
            const float color[4] = {f, 1.0f - f, 0.5f, 1.0f};
            ctx->ClearRenderTargetView(rtv, color);
            hr = sc->Present(1, 0);
            if (FAILED(hr)) {
                std::printf("    %s: %s: Present failed at frame %u: 0x%08lx\n", t, p.name, frame, hr);
                break;
            }
            presented++;
            pump_messages();
        }
        std::printf("    %s: %s: %u of 120 frames presented\n", t, p.name, presented);
        ok = ok && presented == 120;
        ctx->ClearState();
        ctx->Flush();
    }
    DestroyWindow(wnd);
    return result(t, ok);
}

} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // a crash must not swallow the last lines
    g_main_thread = GetCurrentThreadId();
    AddVectoredExceptionHandler(1, report_crash);
    std::printf("d3d11probe built " __DATE__ " " __TIME__ "\n");
    bool present = false;
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--present"))
            present = true;

    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    g_compile = compiler ? reinterpret_cast<D3DCompileFn>(reinterpret_cast<void*>(GetProcAddress(compiler, "D3DCompile")))
                         : nullptr;
    if (!g_compile) {
        std::printf("d3dcompiler_47.dll not found: shaders cannot be compiled\n");
        return 1;
    }
    Ref<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.put()));
    if (FAILED(hr)) {
        std::printf("CreateDXGIFactory1 failed: 0x%08lx\n", hr);
        return 1;
    }
    int failures = 0;
    for (UINT i = 0;; i++) {
        Ref<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, adapter.put()) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        std::printf("adapter %u: %ls (vendor 0x%04x device 0x%04x flags 0x%x)\n", i, desc.Description, desc.VendorId,
                    desc.DeviceId, desc.Flags);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;
        Ref<ID3D11Device> dev;
        Ref<ID3D11DeviceContext> ctx;
        D3D_FEATURE_LEVEL level{};
        hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, dev.put(),
                               &level, ctx.put());
        std::printf("  D3D11CreateDevice: 0x%08lx, feature level 0x%x, vgd3d11.dll %s: %s\n", hr, level,
                    GetModuleHandleW(L"vgd3d11.dll") ? "loaded" : "NOT loaded", SUCCEEDED(hr) ? "PASS" : "FAIL");
        if (FAILED(hr)) {
            failures++;
            continue;
        }
        for (auto test : {clear_test, triangle_test, textured_test, depth_test, compute_test, queries_test,
                          update_test})
            failures += !test(dev, ctx);
        failures += !shared_test(dev, ctx, adapter);
        if (present)
            failures += !present_test(dev, ctx, adapter);
    }
    std::printf("%s\n", failures ? "Some tests FAILED" : "All passed!");
    return failures ? 1 : 0;
}
