// SPDX-License-Identifier: MIT
#include "queue_bridge.h"
#include "device.h"
#include "log.h"

namespace vgd::umd {

namespace {

constexpr DWORD kPollMs = 100;   // how often blocked threads look at the stop flag

} // namespace

QueueBridge::QueueBridge(Device& device, D3D12DDI_HRTCOMMANDQUEUE rt, ID3D12CommandQueue* queue)
    : device_(device), rt_(rt), queue_(queue)
{
}

HRESULT QueueBridge::init()
{
    // A physical (non-virtual) context: yttrium's KMD rejects virtual application contexts. Nothing is ever
    // submitted to it; it only carries the runtime's and our fence operations.
    D3DDDICB_CREATECONTEXT cc{};
    cc.NodeOrdinal = 0;
    cc.EngineAffinity = 1;
    HRESULT hr = device_.um_callbacks->pfnCreateContextCb(rt_, &cc);
    if (FAILED(hr) || !cc.hContext) {
        VGD_WARN("CreateCommandQueue: CreateContextCb failed: 0x%08lx", hr);
        return E_OUTOFMEMORY;
    }
    context_ = cc.hContext;

    for (Com<ID3D12Fence>* f : {&open_, &finished_}) {
        hr = device_.d3d->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), f->put_void());
        if (FAILED(hr)) {
            VGD_WARN("CreateCommandQueue: fence failed: 0x%08lx", hr);
            return E_OUTOFMEMORY;
        }
    }
    if (!create_kernel_fence(&gate_) || !create_kernel_fence(&done_)) {
        VGD_WARN("CreateCommandQueue: no kernel monitored fences; ExecuteCommandLists will wait for the GPU");
        blocking_ = true;
        return S_OK;
    }
    gate_thread_ = std::thread([this] { gate_loop(); });
    completion_thread_ = std::thread([this] { completion_loop(); });
    return S_OK;
}

QueueBridge::~QueueBridge()
{
    {
        std::lock_guard g(lock_);
        stop_ = true;
    }
    cv_.notify_all();
    if (gate_thread_.joinable())
        gate_thread_.join();
    if (completion_thread_.joinable())
        completion_thread_.join();
    // Work still waiting on a gate that never opened would hold the queue forever.
    if (open_)
        open_->Signal(next_);

    const HANDLE dev = device_.rt_device.handle;
    for (D3DKMT_HANDLE f : {gate_, done_})
        if (f) {
            D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT d{f};
            if (FAILED(device_.kt_callbacks->pfnDestroySynchronizationObjectCb(dev, &d)))
                VGD_WARN("DestroyCommandQueue: DestroySynchronizationObjectCb(0x%x) failed", f);
        }
    if (context_) {
        D3DDDICB_DESTROYCONTEXT dc{context_};
        if (FAILED(device_.um_callbacks->pfnDestroyContextCb(rt_, &dc)))
            VGD_WARN("DestroyCommandQueue: DestroyContextCb failed");
    }
}

bool QueueBridge::create_kernel_fence(D3DKMT_HANDLE* out)
{
    D3DDDICB_CREATESYNCHRONIZATIONOBJECT2 c{};
    c.Info.Type = D3DDDI_MONITORED_FENCE;
    c.Info.MonitoredFence.InitialFenceValue = 0;
    c.Info.MonitoredFence.EngineAffinity = 1;
    HRESULT hr = device_.kt_callbacks->pfnCreateSynchronizationObject2Cb(device_.rt_device.handle, &c);
    if (FAILED(hr) || !c.hSyncObject) {
        VGD_WARN("CreateSynchronizationObject2Cb (monitored fence) failed: 0x%08lx", hr);
        return false;
    }
    *out = c.hSyncObject;
    return true;
}

void QueueBridge::execute(UINT count, ID3D12CommandList* const* lists)
{
    const uint64_t value = ++next_;
    if (blocking_) {
        queue_->ExecuteCommandLists(count, lists);
        queue_->Signal(finished_.get(), value);
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (SUCCEEDED(finished_->SetEventOnCompletion(value, event)))
            WaitForSingleObject(event, INFINITE);
        CloseHandle(event);
        return;
    }

    const HANDLE dev = device_.rt_device.handle;
    D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU s{};
    s.hContext = context_;
    s.ObjectCount = 1;
    s.ObjectHandleArray = &gate_;
    s.MonitoredFenceValueArray = &value;
    HRESULT hr = device_.kt_callbacks->pfnSignalSynchronizationObjectFromGpuCb(dev, &s);
    if (FAILED(hr))
        VGD_WARN("ExecuteCommandLists: gate signal %llu failed: 0x%08lx", static_cast<unsigned long long>(value), hr);

    queue_->Wait(open_.get(), value);
    queue_->ExecuteCommandLists(count, lists);
    queue_->Signal(finished_.get(), value);
    {
        std::lock_guard g(lock_);
        gates_.push_back(value);
        completions_.push_back(value);
    }
    cv_.notify_all();

    D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU w{};
    w.hContext = context_;
    w.ObjectCount = 1;
    w.ObjectHandleArray = &done_;
    w.MonitoredFenceValueArray = &value;
    hr = device_.kt_callbacks->pfnWaitForSynchronizationObjectFromGpuCb(dev, &w);
    if (FAILED(hr))
        VGD_WARN("ExecuteCommandLists: done wait %llu failed: 0x%08lx", static_cast<unsigned long long>(value), hr);
}

bool QueueBridge::wait_from_cpu(D3DKMT_HANDLE fence, uint64_t value)
{
    // Asynchronous form, so the thread can still notice the stop flag if the gate never opens.
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU w{};
    w.ObjectCount = 1;
    w.ObjectHandleArray = &fence;
    w.FenceValueArray = &value;
    w.hAsyncEvent = event;
    HRESULT hr = device_.kt_callbacks->pfnWaitForSynchronizationObjectFromCpuCb(device_.rt_device.handle, &w);
    bool ok = SUCCEEDED(hr);
    if (!ok)
        VGD_WARN("queue: CPU wait for gate %llu failed: 0x%08lx", static_cast<unsigned long long>(value), hr);
    while (ok && WaitForSingleObject(event, kPollMs) == WAIT_TIMEOUT) {
        std::lock_guard g(lock_);
        if (stop_) {
            VGD_WARN("queue: destroyed while waiting for gate %llu", static_cast<unsigned long long>(value));
            ok = false;
        }
    }
    CloseHandle(event);
    return ok;
}

void QueueBridge::gate_loop()
{
    for (;;) {
        uint64_t value;
        {
            std::unique_lock g(lock_);
            cv_.wait(g, [this] { return stop_ || !gates_.empty(); });
            if (gates_.empty())
                return;
            value = gates_.front();
            gates_.pop_front();
        }
        wait_from_cpu(gate_, value);   // on failure, open anyway rather than deadlock the runtime
        open_->Signal(value);
    }
}

void QueueBridge::completion_loop()
{
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    for (;;) {
        uint64_t value;
        {
            std::unique_lock g(lock_);
            cv_.wait(g, [this] { return (stop_ && gates_.empty()) || !completions_.empty(); });
            if (completions_.empty())
                break;
            value = completions_.front();
            completions_.pop_front();
        }
        if (SUCCEEDED(finished_->SetEventOnCompletion(value, event)))
            while (WaitForSingleObject(event, kPollMs) == WAIT_TIMEOUT) {
                std::lock_guard g(lock_);
                if (stop_)
                    break;
            }
        D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMCPU s{};
        s.ObjectCount = 1;
        s.ObjectHandleArray = &done_;
        s.FenceValueArray = &value;
        HRESULT hr = device_.kt_callbacks->pfnSignalSynchronizationObjectFromCpuCb(device_.rt_device.handle, &s);
        if (FAILED(hr))
            VGD_WARN("queue: done signal %llu failed: 0x%08lx", static_cast<unsigned long long>(value), hr);
    }
    CloseHandle(event);
}

} // namespace vgd::umd
