// SPDX-License-Identifier: MIT
// Orders a D3D12 queue's Vulkan work against the runtime's fences.
//
// The runtime implements ID3D12CommandQueue::Signal/Wait itself, as GPU signals and waits on the kernel context
// the queue created; our work never runs on that context, it goes to Venus. So each ExecuteCommandLists becomes:
//
//   context:  signal gate=N  ->  wait done>=N  ->  (the runtime's later signals and waits)
//   submit thread:      CPU wait gate>=N, vkQueueSubmit (signals timeline=N)
//   completion thread:  wait timeline>=N, CPU signal done=N
//
// The gate holds our submission back until the runtime's earlier waits on the context are satisfied, and the done
// wait holds the runtime's later signals until our work is complete. Both fences are kernel monitored fences we own.
// If they cannot be created, execution falls back to submitting and waiting for completion on the calling thread.
#pragma once

#include "wdk.h"

#include <vgd/core.h>

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace vgd::umd {

struct Device;

class QueueBridge {
public:
    QueueBridge(Device& device, D3D12DDI_HRTCOMMANDQUEUE rt, vgd::CommandQueue& queue);
    ~QueueBridge();
    // Creates the kernel context the runtime signals and waits on; failure fails queue creation.
    HRESULT init();
    HRESULT execute(vgd::CommandBatch batch);
    // The kernel context the runtime fences on; present happens on it too.
    HANDLE context() const { return context_; }

private:
    struct Job {
        uint64_t value;
        vgd::CommandBatch batch;
    };

    bool create_fence(D3DKMT_HANDLE* out);
    bool wait_from_cpu(D3DKMT_HANDLE fence, uint64_t value);
    void submit_loop();
    void completion_loop();

    Device& device_;
    D3D12DDI_HRTCOMMANDQUEUE rt_;
    vgd::CommandQueue& queue_;
    HANDLE context_ = nullptr;
    D3DKMT_HANDLE gate_ = 0, done_ = 0;
    std::unique_ptr<vgd::Fence> timeline_;
    uint64_t next_ = 0;   // last value handed out; guarded by the runtime's per-queue serialization
    bool blocking_ = false;

    std::mutex lock_;
    std::condition_variable cv_;
    std::deque<Job> submits_;
    std::deque<uint64_t> completions_;
    bool stop_ = false;
    std::thread submit_thread_, completion_thread_;
};

} // namespace vgd::umd
