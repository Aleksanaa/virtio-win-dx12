// SPDX-License-Identifier: MIT
// Plan Phase 3 milestone 2: upload -> copy -> fence -> readback, across the COPY and DIRECT queues.
#include "test_util.h"

#include <vector>

static bool copy_path(vgd::Device& dev)
{
    constexpr uint64_t size = 1 << 20;
    std::unique_ptr<vgd::Resource> upload, gpu, readback;
    CHECK_HR(dev.create_committed_buffer(D3D12_HEAP_TYPE_UPLOAD, size, D3D12_RESOURCE_FLAG_NONE, &upload));
    CHECK_HR(dev.create_committed_buffer(D3D12_HEAP_TYPE_DEFAULT, size, D3D12_RESOURCE_FLAG_NONE, &gpu));
    CHECK_HR(dev.create_committed_buffer(D3D12_HEAP_TYPE_READBACK, size, D3D12_RESOURCE_FLAG_NONE, &readback));
    CHECK(upload->cpu_pointer() && readback->cpu_pointer() && !gpu->cpu_pointer());
    CHECK(gpu->gpu_virtual_address() != 0);

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    std::unique_ptr<vgd::CommandQueue> copy_queue, direct_queue;
    CHECK_HR(dev.create_command_queue(qd, &copy_queue));
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    CHECK_HR(dev.create_command_queue(qd, &direct_queue));

    std::unique_ptr<vgd::CommandAllocator> copy_alloc, direct_alloc;
    CHECK_HR(dev.create_command_allocator(D3D12_COMMAND_LIST_TYPE_COPY, &copy_alloc));
    CHECK_HR(dev.create_command_allocator(D3D12_COMMAND_LIST_TYPE_DIRECT, &direct_alloc));
    std::unique_ptr<vgd::CommandList> copy_list, direct_list;
    CHECK_HR(dev.create_command_list(D3D12_COMMAND_LIST_TYPE_COPY, *copy_alloc, &copy_list));
    CHECK_HR(dev.create_command_list(D3D12_COMMAND_LIST_TYPE_DIRECT, *direct_alloc, &direct_list));

    std::unique_ptr<vgd::Fence> fence, gate;
    CHECK_HR(dev.create_fence(0, &fence));
    CHECK_HR(dev.create_fence(0, &gate));

    // Two rounds: the second reuses allocators and lists after reset, as applications do every frame.
    for (uint32_t round = 0; round < 2; round++) {
        auto* src = static_cast<uint32_t*>(upload->cpu_pointer());
        for (uint64_t i = 0; i < size / 4; i++)
            src[i] = static_cast<uint32_t>(i * 2654435761u) ^ round;
        std::memset(readback->cpu_pointer(), 0, size);

        if (round) {
            CHECK_HR(copy_alloc->reset());
            CHECK_HR(direct_alloc->reset());
            CHECK_HR(copy_list->reset(*copy_alloc));
            CHECK_HR(direct_list->reset(*direct_alloc));
        }
        const uint64_t base = round * 10;

        copy_list->copy_buffer_region(*gpu, 0, *upload, 0, size);
        CHECK_HR(copy_list->close());

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        direct_list->resource_barrier({&barrier, 1});
        // Copy in two halves with swapped order, so offsets are exercised.
        direct_list->copy_buffer_region(*readback, size / 2, *gpu, size / 2, size / 2);
        direct_list->copy_buffer_region(*readback, 0, *gpu, 0, size / 2);
        CHECK_HR(direct_list->close());

        // The copy queue waits for a CPU signal that arrives after submission (wait-before-signal).
        CHECK_HR(copy_queue->wait(*gate, base + 1));
        vgd::CommandList* cl = copy_list.get();
        CHECK_HR(copy_queue->execute_command_lists({&cl, 1}));
        CHECK_HR(copy_queue->signal(*fence, base + 1));

        CHECK_HR(direct_queue->wait(*fence, base + 1));
        cl = direct_list.get();
        CHECK_HR(direct_queue->execute_command_lists({&cl, 1}));
        CHECK_HR(direct_queue->signal(*fence, base + 2));

        CHECK(fence->completed_value() < base + 1);
        CHECK_HR(gate->signal(base + 1));
        CHECK(fence->wait(base + 2, 5'000'000'000ull) == S_OK);
        CHECK(fence->completed_value() == base + 2);

        CHECK(std::memcmp(readback->cpu_pointer(), upload->cpu_pointer(), size) == 0);
    }
    return true;
}

int main() { return vgd_test::run_on_adapters("copy_path", copy_path); }
