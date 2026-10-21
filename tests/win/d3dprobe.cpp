// SPDX-License-Identifier: MIT
// Guest probe: lists DXGI adapters and tries D3D12CreateDevice on each. Plain-text output for the harness.
//   --debug  enable the D3D12 debug layer (needs the "Graphics Tools" optional feature) and print the runtime's
//            messages after each attempt.
//   --copy   on each device created, copy a buffer upload -> default -> readback and check the bytes.
//   --triangle  on each device created, draw off-screen and check pixels (shaders from tests/shaders).
//   --textured  on each device created, sample a texture through descriptor heaps and tables, check pixels.
//   --depth  on each device created, draw with depth/stencil tests and 4x MSAA with a resolve, check pixels.
//   --compute  on each device created, clear UAVs and dispatch on a compute queue, check the values.
#include <windows.h>
#include <dxgi1_4.h>
#include <dxgidebug.h>
#include <directx/d3d12.h>
#include <dxguids/dxguids.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

#include <compute_dxil.h>
#include <depth_dxil.h>
#include <textured_dxil.h>
#include <triangle_dxil.h>

namespace {

IDXGIInfoQueue* g_info = nullptr;
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
    std::printf("ACCESS VIOLATION in %s+0x%llx (ip %p), %s address 0x%llx, thread %lu%s\n", name,
                static_cast<unsigned long long>(static_cast<char*>(r->ExceptionAddress) - reinterpret_cast<char*>(mod)),
                r->ExceptionAddress, r->ExceptionInformation[0] == 1 ? "writing" : r->ExceptionInformation[0] == 8 ? "executing" : "reading",
                static_cast<unsigned long long>(r->ExceptionInformation[1]), GetCurrentThreadId(),
                GetCurrentThreadId() == g_main_thread ? " (main)" : "");
    // A call through a null pointer leaves the caller's return address on top of the stack.
    void* ret = *reinterpret_cast<void**>(e->ContextRecord->Rsp);
    HMODULE rmod = nullptr;
    char rname[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(ret), &rmod))
        GetModuleFileNameA(rmod, rname, sizeof(rname));
    std::printf("  return address on stack: %s+0x%llx (%p)\n", rname,
                static_cast<unsigned long long>(static_cast<char*>(ret) - reinterpret_cast<char*>(rmod)), ret);
    return EXCEPTION_CONTINUE_SEARCH;
}

void enable_debug_layer()
{
    ID3D12Debug* debug = nullptr;
    HRESULT hr = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
    if (FAILED(hr)) {
        std::printf("debug layer unavailable (0x%08lx): install Graphics Tools, see vgd-test.ps1\n", hr);
        return;
    }
    debug->EnableDebugLayer();
    debug->Release();

    using GetDebugInterface1 = HRESULT(WINAPI*)(UINT, REFIID, void**);
    auto get = reinterpret_cast<GetDebugInterface1>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("dxgi.dll"), "DXGIGetDebugInterface1")));
    if (get && SUCCEEDED(get(0, IID_IDXGIInfoQueue, reinterpret_cast<void**>(&g_info))))
        std::printf("debug layer on; runtime messages follow each attempt\n");
    else
        std::printf("debug layer on; messages only in the debugger output (DebugView)\n");
}

void print_messages()
{
    if (!g_info)
        return;
    const UINT64 n = g_info->GetNumStoredMessages(DXGI_DEBUG_ALL);
    for (UINT64 i = 0; i < n; i++) {
        SIZE_T len = 0;
        g_info->GetMessage(DXGI_DEBUG_ALL, i, nullptr, &len);
        std::vector<char> buf(len);
        auto* m = reinterpret_cast<DXGI_INFO_QUEUE_MESSAGE*>(buf.data());
        if (SUCCEEDED(g_info->GetMessage(DXGI_DEBUG_ALL, i, m, &len)))
            std::printf("  [runtime] %.*s\n", static_cast<int>(m->DescriptionByteLength), m->pDescription);
    }
    g_info->ClearStoredMessages(DXGI_DEBUG_ALL);
}

// What the CPU mapping of a buffer really is: a bad Venus/QEMU host-memory mapping shows up here before the write faults.
void describe_mapping(const char* name, void* p, UINT64 size)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) {
        std::printf("  copy: %s mapping %p: VirtualQuery failed (%lu)\n", name, p, GetLastError());
        return;
    }
    std::printf("  copy: %s mapping %p: region %p + 0x%llx, state 0x%lx, protect 0x%lx, type 0x%lx%s\n", name, p,
                mbi.BaseAddress, static_cast<unsigned long long>(mbi.RegionSize), mbi.State, mbi.Protect, mbi.Type,
                static_cast<char*>(mbi.BaseAddress) + mbi.RegionSize < static_cast<char*>(p) + size ? " (SHORTER than the buffer)" : "");
    std::fflush(stdout);
}

// Upload -> default -> readback through one direct queue, with a fence wait in between. Returns true on a match.
bool copy_test(ID3D12Device* device)
{
    constexpr UINT64 kSize = 256 * 1024;
    auto step = [](const char* what, HRESULT hr) {
        std::printf("  copy: %-28s 0x%08lx %s\n", what, hr, SUCCEEDED(hr) ? "ok" : "FAIL");
        print_messages();
        return SUCCEEDED(hr);
    };
    auto buffer = [&](D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, ID3D12Resource** out) {
        D3D12_HEAP_PROPERTIES heap{type};
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = kSize;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                               IID_PPV_ARGS(out));
    };

    ID3D12Resource *upload = nullptr, *gpu = nullptr, *readback = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool ok = false;

    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    void* p = nullptr;
    D3D12_RANGE none{0, 0};
    if (!step("upload buffer", buffer(D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, &upload)) ||
        !step("default buffer", buffer(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, &gpu)) ||
        !step("readback buffer", buffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, &readback)) ||
        !step("command queue", device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
        !step("command allocator",
              device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        !step("command list", device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                                                        IID_PPV_ARGS(&list))) ||
        !step("fence", device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) ||
        !step("map upload", upload->Map(0, &none, &p)))
        goto out;
    std::printf("  copy: gpu va upload 0x%llx default 0x%llx readback 0x%llx\n",
                static_cast<unsigned long long>(upload->GetGPUVirtualAddress()),
                static_cast<unsigned long long>(gpu->GetGPUVirtualAddress()),
                static_cast<unsigned long long>(readback->GetGPUVirtualAddress()));
    describe_mapping("upload", p, kSize);
    for (UINT64 i = 0; i < kSize / 4; i++)
        static_cast<uint32_t*>(p)[i] = static_cast<uint32_t>(i * 2654435761u);
    std::printf("  copy: upload written\n");
    upload->Unmap(0, nullptr);
    std::printf("  copy: upload unmapped\n");

    {
        list->CopyBufferRegion(gpu, 0, upload, 0, kSize);
        std::printf("  copy: recorded upload -> default\n");
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = gpu;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &b);
        std::printf("  copy: recorded barrier\n");
        // Two halves in swapped order, so offsets are exercised.
        list->CopyBufferRegion(readback, kSize / 2, gpu, kSize / 2, kSize / 2);
        list->CopyBufferRegion(readback, 0, gpu, 0, kSize / 2);
        std::printf("  copy: recorded default -> readback\n");
    }
    if (!step("close list", list->Close()))
        goto out;
    {
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        std::printf("  copy: executed\n");
    }
    if (!step("signal fence", queue->Signal(fence, 1)) ||
        !step("set event", fence->SetEventOnCompletion(1, event)))
        goto out;
    if (WaitForSingleObject(event, 5000) != WAIT_OBJECT_0) {
        std::printf("  copy: fence did not reach 1 within 5 s (completed value %llu)\n",
                    static_cast<unsigned long long>(fence->GetCompletedValue()));
        goto out;
    }
    std::printf("  copy: fence completed value %llu\n", static_cast<unsigned long long>(fence->GetCompletedValue()));
    {
        D3D12_RANGE all{0, kSize};
        if (!step("map readback", readback->Map(0, &all, &p)))
            goto out;
        describe_mapping("readback", p, kSize);
        UINT64 bad = 0, first_bad = 0;
        for (UINT64 i = 0; i < kSize / 4; i++)
            if (static_cast<uint32_t*>(p)[i] != static_cast<uint32_t>(i * 2654435761u) && !bad++)
                first_bad = i;
        readback->Unmap(0, &none);
        if (bad)
            std::printf("  copy: %llu of %llu words differ, first at word %llu\n", bad,
                        static_cast<unsigned long long>(kSize / 4), first_bad);
        ok = !bad;
    }
out:
    std::printf("  copy test: %s\n", ok ? "PASS" : "FAIL");
    for (IUnknown* u : std::initializer_list<IUnknown*>{fence, list, alloc, queue, readback, gpu, upload})
        if (u)
            u->Release();
    CloseHandle(event);
    return ok;
}


// An off-screen draw with exact pixels, the same scene as core/tests/triangle.cpp: a clear, a triangle from vertex
// ids in the top-left corner, one from a vertex buffer in the bottom-right corner (colors from root constants), a
// rect clear in the top-right corner, then a copy to a readback buffer.
bool triangle_test(ID3D12Device* device)
{
    constexpr UINT kSize = 64;
    auto step = [](const char* what, HRESULT hr) {
        std::printf("  triangle: %-26s 0x%08lx %s\n", what, hr, SUCCEEDED(hr) ? "ok" : "FAIL");
        print_messages();
        return SUCCEEDED(hr);
    };

    ID3DBlob *rs_blob = nullptr, *rs_error = nullptr;
    ID3D12RootSignature* rs = nullptr;
    ID3D12PipelineState *pso_corner = nullptr, *pso_buffer = nullptr;
    ID3D12Resource *target = nullptr, *vb = nullptr, *readback = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool ok = false;

    D3D12_ROOT_PARAMETER1 param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants = {0, 0, 4};
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd{D3D_ROOT_SIGNATURE_VERSION_1_1};
    rsd.Desc_1_1 = {1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.VS = {k_vs_corner, sizeof(k_vs_corner)};
    pd.PS = {k_ps_color, sizeof(k_ps_color)};
    for (auto& rt : pd.BlendState.RenderTarget)
        rt = {FALSE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_ZERO,
              D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL};
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;   // a winding mix-up culls the triangles
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    const D3D12_INPUT_ELEMENT_DESC element{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                                           D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};

    D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT}, upload_heap{D3D12_HEAP_TYPE_UPLOAD},
        readback_heap{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = td.Height = kSize;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const float clear[4] = {0.0f, 0.0f, 0.6f, 1.0f};
    D3D12_CLEAR_VALUE clear_value{DXGI_FORMAT_R8G8B8A8_UNORM, {clear[0], clear[1], clear[2], clear[3]}};

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 readback_size = 0;
    device->GetCopyableFootprints(&td, 0, 1, 0, &footprint, nullptr, nullptr, &readback_size);
    std::printf("  triangle: footprint offset %llu, %ux%u, row pitch %u, total %llu\n",
                static_cast<unsigned long long>(footprint.Offset), footprint.Footprint.Width,
                footprint.Footprint.Height, footprint.Footprint.RowPitch, static_cast<unsigned long long>(readback_size));
    auto buffer_desc = [](UINT64 size) {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = size;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return d;
    };
    const float vertices[] = {1, 0, 1, -1, 0, -1};   // bottom-right corner, clockwise on screen
    const D3D12_RESOURCE_DESC vb_desc = buffer_desc(sizeof(vertices)), rb_desc = buffer_desc(readback_size);
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    void* p = nullptr;
    D3D12_RANGE none{0, 0};

    HRESULT hr = D3D12SerializeVersionedRootSignature(&rsd, &rs_blob, &rs_error);
    if (!step("serialize root signature", hr) ||
        !step("root signature", device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                                            IID_PPV_ARGS(&rs))))
        goto out;
    pd.pRootSignature = rs;
    if (!step("pipeline (vertex ids)", device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso_corner))))
        goto out;
    pd.VS = {k_vs_buffer, sizeof(k_vs_buffer)};
    pd.InputLayout = {&element, 1};
    if (!step("pipeline (vertex buffer)", device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso_buffer))) ||
        !step("render target", device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &td,
                                                               D3D12_RESOURCE_STATE_RENDER_TARGET, &clear_value,
                                                               IID_PPV_ARGS(&target))) ||
        !step("vertex buffer", device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &vb_desc,
                                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                               IID_PPV_ARGS(&vb))) ||
        !step("readback buffer", device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &rb_desc,
                                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                 IID_PPV_ARGS(&readback))) ||
        !step("rtv heap", device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap))) ||
        !step("command queue", device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
        !step("command allocator",
              device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        !step("command list", device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, pso_corner,
                                                        IID_PPV_ARGS(&list))) ||
        !step("fence", device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) ||
        !step("map vertex buffer", vb->Map(0, &none, &p)))
        goto out;
    std::memcpy(p, vertices, sizeof(vertices));
    vb->Unmap(0, nullptr);

    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv;
        rtv_heap->GetCPUDescriptorHandleForHeapStart(&rtv);   // the MinGW form of the struct-returning method
        device->CreateRenderTargetView(target, nullptr, rtv);
        const float color_a[4] = {1.0f, 0.2f, 0.0f, 1.0f}, color_b[4] = {0.0f, 0.4f, 1.0f, 1.0f};
        const D3D12_VIEWPORT viewport{0, 0, kSize, kSize, 0, 1};
        const D3D12_RECT scissor{0, 0, kSize, kSize}, corner{kSize - 8, 0, kSize, 8};
        const D3D12_VERTEX_BUFFER_VIEW vbv{vb->GetGPUVirtualAddress(), sizeof(vertices), 8};

        list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->SetGraphicsRootSignature(rs);
        list->SetGraphicsRoot32BitConstants(0, 4, color_a, 0);
        list->DrawInstanced(3, 1, 0, 0);
        list->SetPipelineState(pso_buffer);
        list->IASetVertexBuffers(0, 1, &vbv);
        list->SetGraphicsRoot32BitConstants(0, 4, color_b, 0);
        list->DrawInstanced(3, 1, 0, 0);
        list->ClearRenderTargetView(rtv, color_b, 1, &corner);

        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = target;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst{readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}, src{target};
        dst.PlacedFootprint = footprint;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        std::printf("  triangle: recorded\n");
    }
    if (!step("close list", list->Close()))
        goto out;
    {
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
    }
    if (!step("signal fence", queue->Signal(fence, 1)) || !step("set event", fence->SetEventOnCompletion(1, event)))
        goto out;
    if (WaitForSingleObject(event, 5000) != WAIT_OBJECT_0) {
        std::printf("  triangle: fence did not reach 1 within 5 s (completed value %llu)\n",
                    static_cast<unsigned long long>(fence->GetCompletedValue()));
        goto out;
    }
    {
        D3D12_RANGE all{0, static_cast<SIZE_T>(readback_size)};
        if (!step("map readback", readback->Map(0, &all, &p)))
            goto out;
        const auto* bytes = static_cast<const uint8_t*>(p) + footprint.Offset;
        auto at = [&](UINT x, UINT y) {
            uint32_t v;
            std::memcpy(&v, bytes + y * footprint.Footprint.RowPitch + x * 4, 4);
            return v;
        };
        auto rgba = [](uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | g << 8 | b << 16 | a << 24; };
        const uint32_t a = rgba(255, 51, 0, 255), b = rgba(0, 102, 255, 255), c = rgba(0, 0, 153, 255);
        struct Probe { UINT x, y; uint32_t expected; const char* what; } probes[] = {
            {4, 4, a, "top-left corner (vertex id triangle)"},
            {60, 60, b, "bottom-right corner (vertex buffer triangle)"},
            {4, 60, c, "bottom-left (clear)"},
            {32, 32, c, "center (clear)"},
            {60, 2, b, "top-right (rect clear)"},
            {52, 2, c, "next to the rect clear"},
        };
        ok = true;
        for (const Probe& pr : probes) {
            const uint32_t v = at(pr.x, pr.y);
            std::printf("  triangle: pixel (%2u, %2u) 0x%08x %s %s\n", pr.x, pr.y, v,
                        v == pr.expected ? "ok  " : "FAIL", pr.what);
            ok &= v == pr.expected;
        }
        readback->Unmap(0, &none);
    }
out:
    if (rs_error)
        std::printf("  triangle: root signature error: %.*s\n", static_cast<int>(rs_error->GetBufferSize()),
                    static_cast<const char*>(rs_error->GetBufferPointer()));
    std::printf("  triangle test: %s\n", ok ? "PASS" : "FAIL");
    for (IUnknown* u : std::initializer_list<IUnknown*>{fence, list, alloc, queue, rtv_heap, readback, vb, target,
                                                         pso_buffer, pso_corner, rs, rs_error, rs_blob})
        if (u)
            u->Release();
    CloseHandle(event);
    return ok;
}


// Descriptor heaps and tables, the same scene as core/tests/descriptors.cpp: a fullscreen triangle samples a 4x4
// texture (SRV made in a CPU heap and copied to the shader-visible one) through a heap sampler on the left half and a
// static sampler on the right, and adds a table CBV, a structured buffer from the table and a root CBV.
bool textured_test(ID3D12Device* device)
{
    constexpr UINT kSize = 64, kTex = 4;
    auto step = [](const char* what, HRESULT hr) {
        std::printf("  textured: %-26s 0x%08lx %s\n", what, hr, SUCCEEDED(hr) ? "ok" : "FAIL");
        print_messages();
        return SUCCEEDED(hr);
    };
    auto rgba = [](uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | g << 8 | b << 16 | a << 24; };

    ID3DBlob *rs_blob = nullptr, *rs_error = nullptr;
    ID3D12RootSignature* rs = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12Resource *tex = nullptr, *upload = nullptr, *constants = nullptr, *offsets = nullptr, *target = nullptr,
                   *readback = nullptr;
    ID3D12DescriptorHeap *rtv_heap = nullptr, *cpu_heap = nullptr, *gpu_heap = nullptr, *sampler_heap = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool ok = false;

    D3D12_DESCRIPTOR_RANGE1 resource_ranges[2] = {
        {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE,
         D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND},
    };
    D3D12_DESCRIPTOR_RANGE1 sampler_range{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0,
                                          D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0};
    D3D12_ROOT_PARAMETER1 params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = {2, resource_ranges};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {1, &sampler_range};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[2].Descriptor = {1, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE};
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_SAMPLER_DESC point{};
    point.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    point.AddressU = point.AddressV = point.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    point.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_STATIC_SAMPLER_DESC static_sampler{point.Filter, point.AddressU, point.AddressV, point.AddressW, 0, 1,
                                             D3D12_COMPARISON_FUNC_NEVER, D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK,
                                             0, D3D12_FLOAT32_MAX, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL};
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd{D3D_ROOT_SIGNATURE_VERSION_1_1};
    rsd.Desc_1_1 = {3, params, 1, &static_sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.VS = {k_vs_fullscreen, sizeof(k_vs_fullscreen)};
    pd.PS = {k_ps_textured, sizeof(k_ps_textured)};
    for (auto& rt : pd.BlendState.RenderTarget)
        rt = {FALSE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_ZERO,
              D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL};
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;

    D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT}, upload_heap{D3D12_HEAP_TYPE_UPLOAD},
        readback_heap{D3D12_HEAP_TYPE_READBACK};
    auto texture_desc = [](UINT size, D3D12_RESOURCE_FLAGS flags) {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = d.Height = size;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Flags = flags;
        return d;
    };
    auto buffer_desc = [](UINT64 size) {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = size;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return d;
    };
    const D3D12_RESOURCE_DESC tex_desc = texture_desc(kTex, D3D12_RESOURCE_FLAG_NONE),
                              target_desc = texture_desc(kSize, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT tex_fp{}, target_fp{};
    UINT64 upload_size = 0, readback_size = 0;
    device->GetCopyableFootprints(&tex_desc, 0, 1, 0, &tex_fp, nullptr, nullptr, &upload_size);
    device->GetCopyableFootprints(&target_desc, 0, 1, 0, &target_fp, nullptr, nullptr, &readback_size);
    const D3D12_RESOURCE_DESC upload_desc = buffer_desc(upload_size), constants_desc = buffer_desc(512),
                              offsets_desc = buffer_desc(32), readback_desc = buffer_desc(readback_size);
    D3D12_DESCRIPTOR_HEAP_DESC rtv_hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1},
        cpu_hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8},
        gpu_hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE},
        sampler_hd{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    void* p = nullptr;
    D3D12_RANGE none{0, 0};

    HRESULT hr = D3D12SerializeVersionedRootSignature(&rsd, &rs_blob, &rs_error);
    if (!step("serialize root signature", hr) ||
        !step("root signature", device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                                            IID_PPV_ARGS(&rs))))
        goto out;
    pd.pRootSignature = rs;
    if (!step("pipeline", device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso))) ||
        !step("texture", device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex))) ||
        !step("upload buffer", device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                               IID_PPV_ARGS(&upload))) ||
        !step("constants", device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &constants_desc,
                                                           D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                           IID_PPV_ARGS(&constants))) ||
        !step("structured buffer", device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &offsets_desc,
                                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                   IID_PPV_ARGS(&offsets))) ||
        !step("render target", device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
                                                               D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                               IID_PPV_ARGS(&target))) ||
        !step("readback buffer", device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                 IID_PPV_ARGS(&readback))) ||
        !step("rtv heap", device->CreateDescriptorHeap(&rtv_hd, IID_PPV_ARGS(&rtv_heap))) ||
        !step("cpu heap", device->CreateDescriptorHeap(&cpu_hd, IID_PPV_ARGS(&cpu_heap))) ||
        !step("shader-visible heap", device->CreateDescriptorHeap(&gpu_hd, IID_PPV_ARGS(&gpu_heap))) ||
        !step("sampler heap", device->CreateDescriptorHeap(&sampler_hd, IID_PPV_ARGS(&sampler_heap))) ||
        !step("command queue", device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
        !step("command allocator",
              device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        !step("command list", device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, pso,
                                                        IID_PPV_ARGS(&list))) ||
        !step("fence", device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        goto out;

    // Data: texels (x * 64, y * 64, 128, 255); scale (1, 1, 1, 0) at 0 and bias (0, 0, 0, 0.4) at 256; offsets[1].
    if (!step("map upload", upload->Map(0, &none, &p)))
        goto out;
    for (UINT y = 0; y < kTex; y++)
        for (UINT x = 0; x < kTex; x++) {
            const uint32_t texel = rgba(x * 64, y * 64, 128, 255);
            std::memcpy(static_cast<uint8_t*>(p) + tex_fp.Offset + y * tex_fp.Footprint.RowPitch + x * 4, &texel, 4);
        }
    upload->Unmap(0, nullptr);
    if (!step("map constants", constants->Map(0, &none, &p)))
        goto out;
    {
        const float scale[4] = {1, 1, 1, 0}, bias[4] = {0, 0, 0, 0.4f};
        std::memcpy(p, scale, sizeof(scale));
        std::memcpy(static_cast<uint8_t*>(p) + 256, bias, sizeof(bias));
    }
    constants->Unmap(0, nullptr);
    if (!step("map structured buffer", offsets->Map(0, &none, &p)))
        goto out;
    {
        const float data[8] = {0.5f, 0.5f, 0.5f, 0.5f, 0, 0, 0, 0.2f};   // [0] would show a wrong index
        std::memcpy(p, data, sizeof(data));
    }
    offsets->Unmap(0, nullptr);

    {
        const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const UINT sampler_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv, cpu, gpu_cpu, sampler_cpu;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu, sampler_gpu;
        rtv_heap->GetCPUDescriptorHandleForHeapStart(&rtv);   // the MinGW form of the struct-returning methods
        cpu_heap->GetCPUDescriptorHandleForHeapStart(&cpu);
        gpu_heap->GetCPUDescriptorHandleForHeapStart(&gpu_cpu);
        gpu_heap->GetGPUDescriptorHandleForHeapStart(&gpu);
        sampler_heap->GetCPUDescriptorHandleForHeapStart(&sampler_cpu);
        sampler_heap->GetGPUDescriptorHandleForHeapStart(&sampler_gpu);
        std::printf("  textured: increments %u / %u, gpu heap start 0x%llx, sampler heap start 0x%llx\n", inc,
                    sampler_inc, static_cast<unsigned long long>(gpu.ptr),
                    static_cast<unsigned long long>(sampler_gpu.ptr));
        auto at = [](D3D12_CPU_DESCRIPTOR_HANDLE h, UINT i, UINT step_size) {
            return D3D12_CPU_DESCRIPTOR_HANDLE{h.ptr + SIZE_T(i) * step_size};
        };

        // The table starts at slot 5: CBV, texture (copied from the CPU heap), structured buffer. Sampler at slot 2.
        device->CreateShaderResourceView(tex, nullptr, at(cpu, 3, inc));
        device->CopyDescriptorsSimple(1, at(gpu_cpu, 6, inc), at(cpu, 3, inc), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{constants->GetGPUVirtualAddress(), 256};
        device->CreateConstantBufferView(&cbv, at(gpu_cpu, 5, inc));
        D3D12_SHADER_RESOURCE_VIEW_DESC sb{};
        sb.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sb.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sb.Buffer = {0, 2, 16, D3D12_BUFFER_SRV_FLAG_NONE};
        device->CreateShaderResourceView(offsets, &sb, at(gpu_cpu, 7, inc));
        device->CreateSampler(&point, at(sampler_cpu, 2, sampler_inc));
        device->CreateRenderTargetView(target, nullptr, rtv);
        print_messages();

        D3D12_TEXTURE_COPY_LOCATION tex_dst{tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
            tex_src{upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        tex_dst.SubresourceIndex = 0;
        tex_src.PlacedFootprint = tex_fp;
        list->CopyTextureRegion(&tex_dst, 0, 0, 0, &tex_src, nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &b);

        const D3D12_VIEWPORT viewport{0, 0, kSize, kSize, 0, 1};
        const D3D12_RECT scissor{0, 0, kSize, kSize};
        ID3D12DescriptorHeap* heaps[] = {gpu_heap, sampler_heap};
        list->SetDescriptorHeaps(2, heaps);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->SetGraphicsRootSignature(rs);
        list->SetGraphicsRootDescriptorTable(0, {gpu.ptr + 5 * inc});
        list->SetGraphicsRootDescriptorTable(1, {sampler_gpu.ptr + 2 * sampler_inc});
        list->SetGraphicsRootConstantBufferView(2, constants->GetGPUVirtualAddress() + 256);
        list->DrawInstanced(3, 1, 0, 0);

        b.Transition.pResource = target;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst{readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
            src{target, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        dst.PlacedFootprint = target_fp;
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        std::printf("  textured: recorded\n");
    }
    if (!step("close list", list->Close()))
        goto out;
    {
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
    }
    if (!step("signal fence", queue->Signal(fence, 1)) || !step("set event", fence->SetEventOnCompletion(1, event)))
        goto out;
    if (WaitForSingleObject(event, 5000) != WAIT_OBJECT_0) {
        std::printf("  textured: fence did not reach 1 within 5 s (completed value %llu)\n",
                    static_cast<unsigned long long>(fence->GetCompletedValue()));
        goto out;
    }
    {
        D3D12_RANGE all{0, static_cast<SIZE_T>(readback_size)};
        if (!step("map readback", readback->Map(0, &all, &p)))
            goto out;
        const auto* bytes = static_cast<const uint8_t*>(p) + target_fp.Offset;
        ok = true;
        // Pixel (x, y) shows texel (x / 16, y / 16); alpha is 1 * 0 + 0.2 + 0.4.
        for (UINT y : {8u, 24u, 40u, 56u})
            for (UINT x : {8u, 24u, 40u, 56u}) {
                uint32_t v;
                std::memcpy(&v, bytes + y * target_fp.Footprint.RowPitch + x * 4, 4);
                const uint32_t expected = rgba(x / 16 * 64, y / 16 * 64, 128, 153);
                std::printf("  textured: pixel (%2u, %2u) 0x%08x %s %s sampler\n", x, y, v,
                            v == expected ? "ok  " : "FAIL", x < 32 ? "heap" : "static");
                ok &= v == expected;
            }
        readback->Unmap(0, &none);
    }
out:
    if (rs_error)
        std::printf("  textured: root signature error: %.*s\n", static_cast<int>(rs_error->GetBufferSize()),
                    static_cast<const char*>(rs_error->GetBufferPointer()));
    std::printf("  textured test: %s\n", ok ? "PASS" : "FAIL");
    for (IUnknown* u : std::initializer_list<IUnknown*>{fence, list, alloc, queue, sampler_heap, gpu_heap, cpu_heap,
                                                         rtv_heap, readback, target, offsets, constants, upload, tex,
                                                         pso, rs, rs_error, rs_blob})
        if (u)
            u->Release();
    CloseHandle(event);
    return ok;
}


// Depth/stencil and MSAA, the same scene as core/tests/depth_stencil.cpp: full and rect clears of each plane, a
// depth-tested pass, a pass behind it, a stencil-equal pass; then a 4x MSAA draw resolved and read back.
bool depth_test(ID3D12Device* device)
{
    constexpr UINT kSize = 64;
    auto step = [](const char* what, HRESULT hr) {
        std::printf("  depth: %-29s 0x%08lx %s\n", what, hr, SUCCEEDED(hr) ? "ok" : "FAIL");
        print_messages();
        return SUCCEEDED(hr);
    };
    auto rgba = [](uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | g << 8 | b << 16 | a << 24; };

    ID3DBlob *rs_blob = nullptr, *rs_error = nullptr;
    ID3D12RootSignature* rs = nullptr;
    ID3D12PipelineState *pso_depth = nullptr, *pso_stencil = nullptr, *pso_msaa = nullptr;
    ID3D12Resource *target = nullptr, *depth = nullptr, *msaa = nullptr, *resolved = nullptr, *readback = nullptr;
    ID3D12DescriptorHeap *rtv_heap = nullptr, *dsv_heap = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool ok = false;
    const DXGI_FORMAT ds_format = DXGI_FORMAT_D24_UNORM_S8_UINT;

    D3D12_ROOT_PARAMETER1 params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 1};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {1, 0, 4};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd{D3D_ROOT_SIGNATURE_VERSION_1_1};
    rsd.Desc_1_1 = {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.VS = {k_vs_flat, sizeof(k_vs_flat)};
    pd.PS = {k_ps_flat, sizeof(k_ps_flat)};
    for (auto& rt : pd.BlendState.RenderTarget)
        rt = {FALSE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_ZERO,
              D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL};
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.DSVFormat = ds_format;
    pd.SampleDesc.Count = 1;
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT}, readback_heap{D3D12_HEAP_TYPE_READBACK};
    auto texture_desc = [&](DXGI_FORMAT format, UINT samples, D3D12_RESOURCE_FLAGS flags) {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = d.Height = kSize;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = format;
        d.SampleDesc.Count = samples;
        d.Flags = flags;
        return d;
    };
    const D3D12_RESOURCE_DESC target_desc =
                                  texture_desc(DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                              depth_desc = texture_desc(ds_format, 1, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL),
                              msaa_desc =
                                  texture_desc(DXGI_FORMAT_R8G8B8A8_UNORM, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                              resolved_desc = texture_desc(DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D12_RESOURCE_FLAG_NONE);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 readback_size = 0;
    device->GetCopyableFootprints(&target_desc, 0, 1, 0, &fp, nullptr, nullptr, &readback_size);
    D3D12_RESOURCE_DESC rb_desc{};
    rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rb_desc.Width = readback_size;
    rb_desc.Height = 1;
    rb_desc.DepthOrArraySize = 1;
    rb_desc.MipLevels = 1;
    rb_desc.SampleDesc.Count = 1;
    rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_DESCRIPTOR_HEAP_DESC rtv_hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2}, dsv_hd{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    UINT64 fence_value = 0;
    D3D12_RANGE none{0, 0};

    const float clear[4] = {0, 0, 0.6f, 1}, a[4] = {1, 0.2f, 0, 1}, b[4] = {0, 0.4f, 1, 1}, c[4] = {0.2f, 1, 0.2f, 1};
    const uint32_t clear_px = rgba(0, 0, 153, 255), a_px = rgba(255, 51, 0, 255), b_px = rgba(0, 102, 255, 255),
                   c_px = rgba(51, 255, 51, 255);
    const D3D12_VIEWPORT viewport{0, 0, kSize, kSize, 0, 1};
    const D3D12_RECT scissor{0, 0, kSize, kSize};
    const D3D12_RECT top_right{kSize - 16, 0, kSize, 16}, bottom_left{0, kSize - 16, 16, kSize};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{}, msaa_rtv{}, dsv{};

    auto draw = [&](ID3D12PipelineState* pso, float z, const float* color) {
        list->SetPipelineState(pso);
        list->SetGraphicsRoot32BitConstants(0, 1, &z, 0);
        list->SetGraphicsRoot32BitConstants(1, 4, color, 0);
        list->DrawInstanced(3, 1, 0, 0);
    };
    auto barrier = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER br{};
        br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        br.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        list->ResourceBarrier(1, &br);
    };
    auto copy_out = [&](ID3D12Resource* r) {
        D3D12_TEXTURE_COPY_LOCATION dst{readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
            src{r, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        dst.PlacedFootprint = fp;
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    };
    // Closes, executes and waits; then checks pixels of the readback.
    struct Probe { UINT x, y; uint32_t expected; const char* what; };
    auto run_and_check = [&](const char* name, std::initializer_list<Probe> probes) {
        if (!step("close list", list->Close()))
            return false;
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        if (!step("signal fence", queue->Signal(fence, ++fence_value)) ||
            !step("set event", fence->SetEventOnCompletion(fence_value, event)))
            return false;
        if (WaitForSingleObject(event, 5000) != WAIT_OBJECT_0) {
            std::printf("  depth: fence did not reach %llu within 5 s\n", static_cast<unsigned long long>(fence_value));
            return false;
        }
        void* p = nullptr;
        D3D12_RANGE all{0, static_cast<SIZE_T>(readback_size)};
        if (!step("map readback", readback->Map(0, &all, &p)))
            return false;
        bool good = true;
        for (const Probe& pr : probes) {
            uint32_t v;
            std::memcpy(&v, static_cast<uint8_t*>(p) + fp.Offset + pr.y * fp.Footprint.RowPitch + pr.x * 4, 4);
            std::printf("  depth: %s pixel (%2u, %2u) 0x%08x %s %s\n", name, pr.x, pr.y, v,
                        v == pr.expected ? "ok  " : "FAIL", pr.what);
            good &= v == pr.expected;
        }
        readback->Unmap(0, &none);
        return good;
    };

    HRESULT hr = D3D12SerializeVersionedRootSignature(&rsd, &rs_blob, &rs_error);
    if (!step("serialize root signature", hr) ||
        !step("root signature", device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                                            IID_PPV_ARGS(&rs))))
        goto out;
    pd.pRootSignature = rs;
    if (!step("pipeline (depth)", device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso_depth))))
        goto out;
    pd.DepthStencilState.StencilEnable = TRUE;
    pd.DepthStencilState.StencilReadMask = pd.DepthStencilState.StencilWriteMask = 0xff;
    pd.DepthStencilState.FrontFace = pd.DepthStencilState.BackFace = {
        D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_EQUAL};
    if (!step("pipeline (stencil)", device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso_stencil))))
        goto out;
    pd.DepthStencilState = {};
    pd.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pd.SampleDesc.Count = 4;
    if (!step("pipeline (4x MSAA)", device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso_msaa))) ||
        !step("render target", device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
                                                               D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                               IID_PPV_ARGS(&target))) ||
        !step("depth/stencil", device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &depth_desc,
                                                               D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr,
                                                               IID_PPV_ARGS(&depth))) ||
        !step("4x MSAA target", device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &msaa_desc,
                                                                D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                                IID_PPV_ARGS(&msaa))) ||
        !step("resolve target", device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &resolved_desc,
                                                                D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr,
                                                                IID_PPV_ARGS(&resolved))) ||
        !step("readback buffer", device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &rb_desc,
                                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                 IID_PPV_ARGS(&readback))) ||
        !step("rtv heap", device->CreateDescriptorHeap(&rtv_hd, IID_PPV_ARGS(&rtv_heap))) ||
        !step("dsv heap", device->CreateDescriptorHeap(&dsv_hd, IID_PPV_ARGS(&dsv_heap))) ||
        !step("command queue", device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
        !step("command allocator",
              device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        !step("command list", device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                                                        IID_PPV_ARGS(&list))) ||
        !step("fence", device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        goto out;

    rtv_heap->GetCPUDescriptorHandleForHeapStart(&rtv);   // the MinGW form of the struct-returning method
    msaa_rtv = {rtv.ptr + device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)};
    dsv_heap->GetCPUDescriptorHandleForHeapStart(&dsv);
    device->CreateRenderTargetView(target, nullptr, rtv);
    device->CreateRenderTargetView(msaa, nullptr, msaa_rtv);
    device->CreateDepthStencilView(depth, nullptr, dsv);
    print_messages();

    // Depth 1 and stencil 0 everywhere; depth 0 in the top-right corner, stencil 1 in the bottom-left.
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 1, &top_right);
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_STENCIL, 0.0f, 1, 1, &bottom_left);
    list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->SetGraphicsRootSignature(rs);
    list->OMSetStencilRef(1);
    draw(pso_depth, 0.5f, a);     // everywhere but the depth-0 corner
    draw(pso_depth, 0.7f, b);     // behind: nowhere
    draw(pso_stencil, 0.2f, c);   // in front, but only where stencil is 1
    barrier(target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    copy_out(target);
    std::printf("  depth: recorded depth/stencil pass\n");
    ok = run_and_check("depth/stencil", {
        {4, 4, a_px, "depth test passed"},
        {32, 32, a_px, "center"},
        {60, 4, clear_px, "depth rect clear (0) rejects"},
        {60, 20, a_px, "below the depth rect"},
        {4, 60, c_px, "stencil rect"},
        {20, 60, a_px, "next to the stencil rect"},
        {60, 60, a_px, "bottom right"},
    });

    if (!step("reset allocator", alloc->Reset()) || !step("reset list", list->Reset(alloc, nullptr)))
        goto out;
    list->ClearRenderTargetView(msaa_rtv, clear, 0, nullptr);
    list->OMSetRenderTargets(1, &msaa_rtv, FALSE, nullptr);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->SetGraphicsRootSignature(rs);
    draw(pso_msaa, 0.5f, b);
    barrier(msaa, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    list->ResolveSubresource(resolved, 0, msaa, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    barrier(resolved, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    copy_out(resolved);
    std::printf("  depth: recorded MSAA pass\n");
    ok = run_and_check("MSAA", {{4, 4, b_px, "resolved"}, {60, 60, b_px, "resolved"}}) && ok;
out:
    if (rs_error)
        std::printf("  depth: root signature error: %.*s\n", static_cast<int>(rs_error->GetBufferSize()),
                    static_cast<const char*>(rs_error->GetBufferPointer()));
    std::printf("  depth test: %s\n", ok ? "PASS" : "FAIL");
    for (IUnknown* u : std::initializer_list<IUnknown*>{fence, list, alloc, queue, dsv_heap, rtv_heap, readback,
                                                         resolved, msaa, depth, target, pso_msaa, pso_stencil,
                                                         pso_depth, rs, rs_error, rs_blob})
        if (u)
            u->Release();
    CloseHandle(event);
    return ok;
}


// Compute and UAVs on a compute queue, the same work as core/tests/compute.cpp: UAV clears of a structured buffer, a
// typed buffer and a texture, then a dispatch writing all three through a root UAV and a descriptor table.
bool compute_test(ID3D12Device* device)
{
    constexpr UINT kElements = 512, kWritten = 256, kImage = 32;
    auto step = [](const char* what, HRESULT hr) {
        std::printf("  compute: %-27s 0x%08lx %s\n", what, hr, SUCCEEDED(hr) ? "ok" : "FAIL");
        print_messages();
        return SUCCEEDED(hr);
    };
    auto rgba = [](uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | g << 8 | b << 16 | a << 24; };

    ID3DBlob *rs_blob = nullptr, *rs_error = nullptr;
    ID3D12RootSignature* rs = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12Resource *structured = nullptr, *typed = nullptr, *image = nullptr, *rb_structured = nullptr,
                   *rb_typed = nullptr, *rb_image = nullptr;
    ID3D12DescriptorHeap *gpu_heap = nullptr, *cpu_heap = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool ok = false;

    D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 1, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0};
    D3D12_ROOT_PARAMETER1 params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 2};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE};
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable = {1, &range};
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd{D3D_ROOT_SIGNATURE_VERSION_1_1};
    rsd.Desc_1_1 = {3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.CS = {k_cs_fill, sizeof(k_cs_fill)};

    D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT}, readback_heap{D3D12_HEAP_TYPE_READBACK};
    auto buffer_desc = [](UINT64 size, D3D12_RESOURCE_FLAGS flags) {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = size;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = flags;
        return d;
    };
    D3D12_RESOURCE_DESC image_desc{};
    image_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    image_desc.Width = image_desc.Height = kImage;
    image_desc.DepthOrArraySize = 1;
    image_desc.MipLevels = 1;
    image_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    image_desc.SampleDesc.Count = 1;
    image_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 image_bytes = 0;
    device->GetCopyableFootprints(&image_desc, 0, 1, 0, &fp, nullptr, nullptr, &image_bytes);
    const D3D12_RESOURCE_DESC uav_desc = buffer_desc(kElements * 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                              rb_desc = buffer_desc(kElements * 4, D3D12_RESOURCE_FLAG_NONE),
                              rb_image_desc = buffer_desc(image_bytes, D3D12_RESOURCE_FLAG_NONE);
    D3D12_DESCRIPTOR_HEAP_DESC gpu_hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE},
        cpu_hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4};
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_COMPUTE};

    HRESULT hr = D3D12SerializeVersionedRootSignature(&rsd, &rs_blob, &rs_error);
    if (!step("serialize root signature", hr) ||
        !step("root signature", device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                                            IID_PPV_ARGS(&rs))))
        goto out;
    pd.pRootSignature = rs;
    if (!step("pipeline", device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso))) ||
        !step("structured buffer", device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &uav_desc,
                                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                                   IID_PPV_ARGS(&structured))) ||
        !step("typed buffer", device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &uav_desc,
                                                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                              IID_PPV_ARGS(&typed))) ||
        !step("texture", device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &image_desc,
                                                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                         IID_PPV_ARGS(&image))) ||
        !step("readback (structured)", device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &rb_desc,
                                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                       IID_PPV_ARGS(&rb_structured))) ||
        !step("readback (typed)", device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &rb_desc,
                                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                  IID_PPV_ARGS(&rb_typed))) ||
        !step("readback (texture)", device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE,
                                                                    &rb_image_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                                    nullptr, IID_PPV_ARGS(&rb_image))) ||
        !step("shader-visible heap", device->CreateDescriptorHeap(&gpu_hd, IID_PPV_ARGS(&gpu_heap))) ||
        !step("cpu heap", device->CreateDescriptorHeap(&cpu_hd, IID_PPV_ARGS(&cpu_heap))) ||
        !step("compute queue", device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
        !step("command allocator",
              device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&alloc))) ||
        !step("command list", device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, alloc, pso,
                                                        IID_PPV_ARGS(&list))) ||
        !step("fence", device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        goto out;

    {
        const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu, gpu_cpu;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu;
        cpu_heap->GetCPUDescriptorHandleForHeapStart(&cpu);   // the MinGW form of the struct-returning methods
        gpu_heap->GetCPUDescriptorHandleForHeapStart(&gpu_cpu);
        gpu_heap->GetGPUDescriptorHandleForHeapStart(&gpu);
        auto c = [&](UINT i) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + SIZE_T(i) * inc}; };
        auto g = [&](UINT i) { return D3D12_GPU_DESCRIPTOR_HANDLE{gpu.ptr + UINT64(i) * inc}; };

        // UAVs in the CPU heap: 0 structured, 1 typed (the table starts here), 2 texture; mirrored in the
        // shader-visible heap, which the clears need too.
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Format = DXGI_FORMAT_UNKNOWN;
        uav.Buffer = {0, kElements, 4, 0, D3D12_BUFFER_UAV_FLAG_NONE};
        device->CreateUnorderedAccessView(structured, nullptr, &uav, c(0));
        uav.Format = DXGI_FORMAT_R32_UINT;
        uav.Buffer.StructureByteStride = 0;
        device->CreateUnorderedAccessView(typed, nullptr, &uav, c(1));
        device->CreateUnorderedAccessView(image, nullptr, nullptr, c(2));
        device->CopyDescriptorsSimple(3, gpu_cpu, cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        print_messages();

        ID3D12DescriptorHeap* heaps[] = {gpu_heap};
        list->SetDescriptorHeaps(1, heaps);
        const UINT beef[4] = {0xdeadbeef, 0, 0, 0}, seven[4] = {7, 0, 0, 0};
        const float blue[4] = {0, 0, 1, 1};
        list->ClearUnorderedAccessViewUint(g(0), c(0), structured, beef, 0, nullptr);
        list->ClearUnorderedAccessViewUint(g(1), c(1), typed, seven, 0, nullptr);
        list->ClearUnorderedAccessViewFloat(g(2), c(2), image, blue, 0, nullptr);
        D3D12_RESOURCE_BARRIER uav_barrier{};
        uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        list->ResourceBarrier(1, &uav_barrier);

        list->SetComputeRootSignature(rs);
        const UINT params_data[2] = {3, 5};
        list->SetComputeRoot32BitConstants(0, 2, params_data, 0);
        list->SetComputeRootUnorderedAccessView(1, structured->GetGPUVirtualAddress());
        list->SetComputeRootDescriptorTable(2, g(1));
        list->Dispatch(kWritten / 64, 1, 1);

        D3D12_RESOURCE_BARRIER b[3] = {};
        for (int i = 0; i < 3; i++) {
            b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b[i].Transition = {i == 0 ? structured : i == 1 ? typed : image, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
        }
        list->ResourceBarrier(3, b);
        list->CopyBufferRegion(rb_structured, 0, structured, 0, kElements * 4);
        list->CopyBufferRegion(rb_typed, 0, typed, 0, kElements * 4);
        D3D12_TEXTURE_COPY_LOCATION dst{rb_image, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
            src{image, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        dst.PlacedFootprint = fp;
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        std::printf("  compute: recorded\n");
    }
    if (!step("close list", list->Close()))
        goto out;
    {
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
    }
    if (!step("signal fence", queue->Signal(fence, 1)) || !step("set event", fence->SetEventOnCompletion(1, event)))
        goto out;
    if (WaitForSingleObject(event, 5000) != WAIT_OBJECT_0) {
        std::printf("  compute: fence did not reach 1 within 5 s\n");
        goto out;
    }
    {
        void *ps = nullptr, *pt = nullptr, *pi = nullptr;
        D3D12_RANGE all{0, kElements * 4}, all_image{0, static_cast<SIZE_T>(image_bytes)}, none{0, 0};
        if (!step("map readbacks", rb_structured->Map(0, &all, &ps)) || FAILED(rb_typed->Map(0, &all, &pt)) ||
            FAILED(rb_image->Map(0, &all_image, &pi)))
            goto out;
        UINT bad_structured = 0, bad_typed = 0, bad_image = 0;
        for (UINT i = 0; i < kElements; i++) {
            bad_structured += static_cast<uint32_t*>(ps)[i] != (i < kWritten ? i * 3 + 5 : 0xdeadbeef);
            bad_typed += static_cast<uint32_t*>(pt)[i] != (i < kWritten ? i + 1000 : 7);
        }
        for (UINT y = 0; y < kImage; y++)
            for (UINT x = 0; x < kImage; x++) {
                uint32_t v;
                std::memcpy(&v, static_cast<uint8_t*>(pi) + fp.Offset + y * fp.Footprint.RowPitch + x * 4, 4);
                bad_image += v != (x < 16 && y < 16 ? rgba(x * 17, y * 17, 51, 255) : rgba(0, 0, 255, 255));
            }
        std::printf("  compute: structured[0..2] %u %u %u, [300] 0x%08x; typed[0] %u, [300] %u\n",
                    static_cast<uint32_t*>(ps)[0], static_cast<uint32_t*>(ps)[1], static_cast<uint32_t*>(ps)[2],
                    static_cast<uint32_t*>(ps)[300], static_cast<uint32_t*>(pt)[0], static_cast<uint32_t*>(pt)[300]);
        std::printf("  compute: wrong values: root UAV %u, table typed UAV %u, table texture UAV %u texels\n",
                    bad_structured, bad_typed, bad_image);
        ok = !bad_structured && !bad_typed && !bad_image;
        rb_image->Unmap(0, &none);
        rb_typed->Unmap(0, &none);
        rb_structured->Unmap(0, &none);
    }
out:
    if (rs_error)
        std::printf("  compute: root signature error: %.*s\n", static_cast<int>(rs_error->GetBufferSize()),
                    static_cast<const char*>(rs_error->GetBufferPointer()));
    std::printf("  compute test: %s\n", ok ? "PASS" : "FAIL");
    for (IUnknown* u : std::initializer_list<IUnknown*>{fence, list, alloc, queue, cpu_heap, gpu_heap, rb_image,
                                                         rb_typed, rb_structured, image, typed, structured, pso, rs,
                                                         rs_error, rs_blob})
        if (u)
            u->Release();
    CloseHandle(event);
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // a crash must not swallow the last lines
    g_main_thread = GetCurrentThreadId();
    AddVectoredExceptionHandler(1, report_crash);
    std::printf("d3dprobe built " __DATE__ " " __TIME__ ", args:");
    for (int i = 1; i < argc; i++)
        std::printf(" %s", argv[i]);
    std::printf("\n");
    bool copy = false, triangle = false, textured = false, depth = false, compute = false;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--debug"))
            enable_debug_layer();
        else if (!std::strcmp(argv[i], "--copy"))
            copy = true;
        else if (!std::strcmp(argv[i], "--triangle"))
            triangle = true;
        else if (!std::strcmp(argv[i], "--textured"))
            textured = true;
        else if (!std::strcmp(argv[i], "--depth"))
            depth = true;
        else if (!std::strcmp(argv[i], "--compute"))
            compute = true;
    }

    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        std::printf("CreateDXGIFactory1 failed: 0x%08lx\n", hr);
        return 1;
    }
    int failures = 0;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        std::printf("adapter %u: %ls (vendor 0x%04x device 0x%04x subsys 0x%08x flags 0x%x)\n", i, desc.Description,
                    desc.VendorId, desc.DeviceId, desc.SubSysId, desc.Flags);

        ID3D12Device* device = nullptr;
        hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        std::printf("  D3D12CreateDevice(FL 11_0): 0x%08lx %s\n", hr, SUCCEEDED(hr) ? "PASS" : "FAIL");
        print_messages();
        if (device) {
            D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_0};
            device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));
            std::printf("  highest shader model: 0x%x\n", sm.HighestShaderModel);
            if (copy && !copy_test(device))
                failures++;
            if (triangle && !triangle_test(device))
                failures++;
            if (textured && !textured_test(device))
                failures++;
            if (depth && !depth_test(device))
                failures++;
            if (compute && !compute_test(device))
                failures++;
            device->Release();
        } else if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            failures++;
        }
        adapter->Release();
    }
    factory->Release();
    return failures ? 1 : 0;
}
