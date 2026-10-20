// SPDX-License-Identifier: MIT
// Guest probe: lists DXGI adapters and tries D3D12CreateDevice on each. Plain-text output for the harness.
//   --debug  enable the D3D12 debug layer (needs the "Graphics Tools" optional feature) and print the runtime's
//            messages after each attempt.
//   --copy   on each device created, copy a buffer upload -> default -> readback and check the bytes.
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
    bool copy = false;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--debug"))
            enable_debug_layer();
        else if (!std::strcmp(argv[i], "--copy"))
            copy = true;
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
            device->Release();
        } else if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            failures++;
        }
        adapter->Release();
    }
    factory->Release();
    return failures ? 1 : 0;
}
