// SPDX-License-Identifier: MIT
#include "internal.h"

namespace vgd {

// --- Allocators --------------------------------------------------------------------------------------------

CommandAllocator::Impl::~Impl()
{
    if (pool)
        device->vk.vkDestroyCommandPool(device->device, pool, nullptr);
}

HRESULT CommandAllocator::Impl::acquire(VkCommandBuffer* out)
{
    if (next < buffers.size()) {
        *out = buffers[next++];
        return S_OK;
    }
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VkResult r = device->vk.vkAllocateCommandBuffers(device->device, &ai, &cmd);
    if (r != VK_SUCCESS)
        return hresult_from_vk(r);
    buffers.push_back(cmd);
    next = buffers.size();
    *out = cmd;
    return S_OK;
}

CommandAllocator::CommandAllocator(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
CommandAllocator::~CommandAllocator() = default;

HRESULT CommandAllocator::reset()
{
    VkResult r = impl_->device->vk.vkResetCommandPool(impl_->device->device, impl_->pool, 0);
    impl_->next = 0;
    return hresult_from_vk(r);
}

HRESULT Device::create_command_allocator(D3D12_COMMAND_LIST_TYPE type, std::unique_ptr<CommandAllocator>* out)
{
    if (!out)
        return E_INVALIDARG;
    auto a = std::make_unique<CommandAllocator::Impl>();
    a->device = impl_.get();
    a->type = type;
    VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    ci.queueFamilyIndex = impl_->slot_for(type).family;
    VkResult r = impl_->vk.vkCreateCommandPool(impl_->device, &ci, nullptr, &a->pool);
    if (r != VK_SUCCESS)
        return hresult_from_vk(r);
    out->reset(new CommandAllocator(std::move(a)));
    return S_OK;
}

// --- Command lists -----------------------------------------------------------------------------------------

HRESULT CommandList::Impl::begin(CommandAllocator& allocator)
{
    if (allocator.impl().type != type)
        return E_INVALIDARG;
    HRESULT hr = allocator.impl().acquire(&cmd);
    if (FAILED(hr))
        return hr;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkResult r = device->vk.vkBeginCommandBuffer(cmd, &bi);
    if (r != VK_SUCCESS)
        return hresult_from_vk(r);
    open = true;
    error = S_OK;
    return S_OK;
}

CommandList::CommandList(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
CommandList::~CommandList() = default;

HRESULT Device::create_command_list(D3D12_COMMAND_LIST_TYPE type, CommandAllocator& allocator,
                                    std::unique_ptr<CommandList>* out)
{
    if (!out)
        return E_INVALIDARG;
    auto l = std::make_unique<CommandList::Impl>();
    l->device = impl_.get();
    l->type = type;
    HRESULT hr = l->begin(allocator);
    if (FAILED(hr))
        return hr;
    out->reset(new CommandList(std::move(l)));
    return S_OK;
}

HRESULT Device::create_command_list(D3D12_COMMAND_LIST_TYPE type, std::unique_ptr<CommandList>* out)
{
    if (!out)
        return E_INVALIDARG;
    auto l = std::make_unique<CommandList::Impl>();
    l->device = impl_.get();
    l->type = type;
    out->reset(new CommandList(std::move(l)));
    return S_OK;
}

HRESULT CommandList::close()
{
    if (!impl_->open)
        return E_FAIL;
    impl_->open = false;
    VkResult r = impl_->device->vk.vkEndCommandBuffer(impl_->cmd);
    return FAILED(impl_->error) ? impl_->error : hresult_from_vk(r);
}

HRESULT CommandList::reset(CommandAllocator& allocator)
{
    if (impl_->open)
        return E_FAIL;
    return impl_->begin(allocator);
}

void CommandList::copy_buffer_region(Resource& dst, uint64_t dst_offset, Resource& src, uint64_t src_offset,
                                     uint64_t bytes)
{
    VkBufferCopy region{src_offset, dst_offset, bytes};
    impl_->device->vk.vkCmdCopyBuffer(impl_->cmd, src.impl().buffer, dst.impl().buffer, 1, &region);
}

void CommandList::resource_barrier(std::span<const D3D12_RESOURCE_BARRIER> barriers)
{
    if (barriers.empty())
        return;
    // First version: every legacy barrier batch is a full memory dependency. Image layouts and split
    // barriers come with textures.
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    impl_->device->vk.vkCmdPipelineBarrier2(impl_->cmd, &dep);
}

// --- Queues ------------------------------------------------------------------------------------------------

CommandQueue::CommandQueue(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
CommandQueue::~CommandQueue() = default;

HRESULT Device::create_command_queue(const D3D12_COMMAND_QUEUE_DESC& desc, std::unique_ptr<CommandQueue>* out)
{
    if (!out)
        return E_INVALIDARG;
    if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT && desc.Type != D3D12_COMMAND_LIST_TYPE_COMPUTE &&
        desc.Type != D3D12_COMMAND_LIST_TYPE_COPY)
        return E_NOTIMPL;
    auto q = std::make_unique<CommandQueue::Impl>();
    q->device = impl_.get();
    q->slot = &impl_->slot_for(desc.Type);
    q->desc = desc;
    out->reset(new CommandQueue(std::move(q)));
    return S_OK;
}

HRESULT CommandQueue::Impl::submit(std::span<const VkCommandBufferSubmitInfo> cmds, const VkSemaphoreSubmitInfo* signal)
{
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    std::lock_guard lock(slot->submit_lock);
    si.waitSemaphoreInfoCount = static_cast<uint32_t>(pending_waits.size());
    si.pWaitSemaphoreInfos = pending_waits.data();
    si.commandBufferInfoCount = static_cast<uint32_t>(cmds.size());
    si.pCommandBufferInfos = cmds.data();
    si.signalSemaphoreInfoCount = signal ? 1 : 0;
    si.pSignalSemaphoreInfos = signal;
    VkResult r = device->vk.vkQueueSubmit2(slot->queue, 1, &si, VK_NULL_HANDLE);
    pending_waits.clear();
    return hresult_from_vk(r);
}

HRESULT CommandQueue::execute_command_lists(std::span<CommandList* const> lists)
{
    std::vector<VkCommandBufferSubmitInfo> cmds;
    cmds.reserve(lists.size());
    for (CommandList* l : lists) {
        if (!l || l->impl().open)
            return E_INVALIDARG;
        VkCommandBufferSubmitInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        ci.commandBuffer = l->impl().cmd;
        cmds.push_back(ci);
    }
    return impl_->submit(cmds, nullptr);
}

HRESULT CommandQueue::signal(Fence& fence, uint64_t value)
{
    VkSemaphoreSubmitInfo s{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    s.semaphore = fence.impl().timeline;
    s.value = value;
    s.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    return impl_->submit({}, &s);
}

HRESULT CommandQueue::wait(Fence& fence, uint64_t value)
{
    // A Vulkan semaphore wait only orders its own batch, so it rides on this queue's next submission.
    // That submission (and, as drivers process a queue in order, everything after it) then waits.
    VkSemaphoreSubmitInfo w{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    w.semaphore = fence.impl().timeline;
    w.value = value;
    w.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    std::lock_guard lock(impl_->slot->submit_lock);
    impl_->pending_waits.push_back(w);
    return S_OK;
}

} // namespace vgd
