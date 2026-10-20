// SPDX-License-Identifier: MIT
#include "queue_bridge.h"
#include "device.h"
#include "log.h"

namespace vgd::umd {

namespace {

constexpr DWORD kPollMs = 100;                       // how often blocked threads look at the stop flag
constexpr uint64_t kPollNs = 100'000'000ull;

} // namespace

QueueBridge::QueueBridge(Device& device, D3D12DDI_HRTCOMMANDQUEUE rt, vgd::CommandQueue& queue)
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

    hr = device_.core->create_fence(0, &timeline_);
    if (FAILED(hr)) {
        VGD_WARN("CreateCommandQueue: timeline semaphore failed: 0x%08lx", hr);
        return E_OUTOFMEMORY;
    }
    if (!create_fence(&gate_) || !create_fence(&done_)) {
        VGD_WARN("CreateCommandQueue: no kernel monitored fences; ExecuteCommandLists will wait for the GPU");
        blocking_ = true;
        return S_OK;
    }
    submit_thread_ = std::thread([this] { submit_loop(); });
    completion_thread_ = std::thread([this] { completion_loop(); });
    VGD_TRACE("CreateCommandQueue: context %p, gate fence 0x%x, done fence 0x%x", context_, gate_, done_);
    return S_OK;
}

QueueBridge::~QueueBridge()
{
    {
        std::lock_guard g(lock_);
        stop_ = true;
    }
    cv_.notify_all();
    if (submit_thread_.joinable())
        submit_thread_.join();
    if (completion_thread_.joinable())
        completion_thread_.join();
    if (timeline_)
        timeline_->wait(next_, 1'000'000'000ull);   // the runtime waits for idle first; this is a safety net

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

bool QueueBridge::create_fence(D3DKMT_HANDLE* out)
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

HRESULT QueueBridge::execute(vgd::CommandBatch batch)
{
    const uint64_t value = ++next_;
    if (blocking_) {
        HRESULT hr = queue_.submit(batch, timeline_.get(), value);
        if (FAILED(hr))
            return hr;
        return timeline_->wait(value, UINT64_MAX);
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

    {
        std::lock_guard g(lock_);
        submits_.push_back({value, std::move(batch)});
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
    return S_OK;
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

void QueueBridge::submit_loop()
{
    for (;;) {
        Job job;
        {
            std::unique_lock g(lock_);
            cv_.wait(g, [this] { return stop_ || !submits_.empty(); });
            if (submits_.empty())
                return;
            job = std::move(submits_.front());
            submits_.pop_front();
        }
        wait_from_cpu(gate_, job.value);   // on failure, submit anyway rather than deadlock the runtime
        HRESULT hr = queue_.submit(job.batch, timeline_.get(), job.value);
        if (FAILED(hr)) {
            VGD_WARN("queue: submit %llu failed: 0x%08lx", static_cast<unsigned long long>(job.value), hr);
            timeline_->signal(job.value);   // keep later fences moving; device removal comes later
        }
        {
            std::lock_guard g(lock_);
            completions_.push_back(job.value);
        }
        cv_.notify_all();
    }
}

void QueueBridge::completion_loop()
{
    for (;;) {
        uint64_t value;
        {
            std::unique_lock g(lock_);
            cv_.wait(g, [this] { return (stop_ && submits_.empty()) || !completions_.empty(); });
            if (completions_.empty())
                return;
            value = completions_.front();
            completions_.pop_front();
        }
        while (timeline_->wait(value, kPollNs) == S_FALSE) {
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
}

} // namespace vgd::umd
