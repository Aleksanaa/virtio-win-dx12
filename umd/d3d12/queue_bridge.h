// SPDX-License-Identifier: MIT
// Orders a D3D12 queue's work on vkd3d-proton against the runtime's fences.
//
// The runtime implements ID3D12CommandQueue::Signal/Wait itself, as GPU signals and waits on the kernel context the
// queue created; our work never runs on that context, it goes to vkd3d-proton's queue. So each ExecuteCommandLists
// becomes:
//
//   context:          signal gate=N  ->  wait done>=N  ->  (the runtime's later signals and waits)
//   vkd3d queue:      Wait(open, N), ExecuteCommandLists, Signal(finished, N)
//   gate thread:      CPU wait gate>=N, then open.Signal(N)
//   completion thread: wait finished>=N, then CPU signal done=N
//
// The gate holds our work back until the runtime's earlier waits on the context are satisfied, and the done wait
// holds the runtime's later signals until our work is complete. gate and done are kernel monitored fences we own,
// open and finished vkd3d-proton fences. The lists are submitted in the call itself (the application may reset them
// as soon as it returns); vkd3d-proton keeps them queued behind the wait. If the kernel fences cannot be created,
// execution falls back to submitting and waiting for completion on the calling thread.
#pragma once

#include "com.h"
#include "wdk.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace vgd::umd {

struct Device;

class QueueBridge {
public:
    QueueBridge(Device& device, D3D12DDI_HRTCOMMANDQUEUE rt, ID3D12CommandQueue* queue);
    ~QueueBridge();
    // Creates the kernel context the runtime signals and waits on; failure fails queue creation.
    HRESULT init();
    void execute(UINT count, ID3D12CommandList* const* lists);
    // The kernel context the runtime fences on; present happens on it too.
    HANDLE context() const { return context_; }

private:
    bool create_kernel_fence(D3DKMT_HANDLE* out);
    bool wait_from_cpu(D3DKMT_HANDLE fence, uint64_t value);
    void gate_loop();
    void completion_loop();

    Device& device_;
    D3D12DDI_HRTCOMMANDQUEUE rt_;
    ID3D12CommandQueue* queue_;
    HANDLE context_ = nullptr;
    D3DKMT_HANDLE gate_ = 0, done_ = 0;
    Com<ID3D12Fence> open_, finished_;
    uint64_t next_ = 0;   // last value handed out; guarded by the runtime's per-queue serialization
    bool blocking_ = false;

    std::mutex lock_;
    std::condition_variable cv_;
    std::deque<uint64_t> gates_, completions_;
    bool stop_ = false;
    std::thread gate_thread_, completion_thread_;
};

} // namespace vgd::umd
