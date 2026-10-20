// SPDX-License-Identifier: MIT
#pragma once

#include <vgd/core.h>

#include <cstdarg>
#include <mutex>

namespace vgd {

void log(LogLevel level, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

HRESULT hresult_from_vk(VkResult r);

struct InstanceDispatch {
#define VGD_VK_GLOBAL(n) PFN_##n n = nullptr;
#define VGD_VK_INSTANCE(n) PFN_##n n = nullptr;
#define VGD_VK_DEVICE(n)
#include "vk_procs.inc"
#undef VGD_VK_GLOBAL
#undef VGD_VK_INSTANCE
#undef VGD_VK_DEVICE
};

struct DeviceDispatch {
#define VGD_VK_GLOBAL(n)
#define VGD_VK_INSTANCE(n)
#define VGD_VK_DEVICE(n) PFN_##n n = nullptr;
#include "vk_procs.inc"
#undef VGD_VK_GLOBAL
#undef VGD_VK_INSTANCE
#undef VGD_VK_DEVICE
};

struct PhysicalDevice {
    VkPhysicalDevice handle = VK_NULL_HANDLE;
    AdapterDesc desc;
    VkPhysicalDeviceMemoryProperties memory{};
    std::vector<VkQueueFamilyProperties> queue_families;
    bool needs_maintenance5_ext = false;   // device is 1.3: maintenance5 is an extension
};

struct Instance::Impl {
    InstanceDispatch vk;
    VkInstance instance = VK_NULL_HANDLE;
    std::vector<PhysicalDevice> physical_devices;
    std::vector<AdapterDesc> adapters;
    ~Impl();
};

// One VkQueue may back several D3D12 queues of the same type, so submission is serialized here.
struct QueueSlot {
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = UINT32_MAX;
    std::mutex submit_lock;
};

struct Device::Impl {
    const InstanceDispatch* ivk = nullptr;
    DeviceDispatch vk;
    VkDevice device = VK_NULL_HANDLE;
    const PhysicalDevice* physical = nullptr;
    QueueSlot direct, compute, copy;
    std::vector<uint32_t> families;   // distinct families in use, for concurrent sharing
    ~Impl();

    QueueSlot& slot_for(D3D12_COMMAND_LIST_TYPE type);
    uint32_t find_memory_type(uint32_t type_bits, D3D12_HEAP_TYPE heap_type) const;
    // The Vulkan format this device stores a DXGI format as: vk_format() plus device-specific substitutions
    // (D24S8 becomes D32S8 where the GPU lacks it, e.g. AMD).
    VkFormat resolve_format(DXGI_FORMAT format) const;
    bool d24s8_supported = false;
};

struct Resource::Impl {
    Device::Impl* device = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint64_t size = 0;
    VkDeviceAddress address = 0;
    void* mapped = nullptr;
    ~Impl();
};

struct Fence::Impl {
    Device::Impl* device = nullptr;
    VkSemaphore timeline = VK_NULL_HANDLE;
    ~Impl();
};

struct CommandAllocator::Impl {
    Device::Impl* device = nullptr;
    D3D12_COMMAND_LIST_TYPE type{};
    VkCommandPool pool = VK_NULL_HANDLE;
    // Command buffers handed out since the last reset; recycled in order after vkResetCommandPool.
    std::vector<VkCommandBuffer> buffers;
    size_t next = 0;
    ~Impl();

    HRESULT acquire(VkCommandBuffer* out);
};

struct CommandList::Impl {
    Device::Impl* device = nullptr;
    D3D12_COMMAND_LIST_TYPE type{};
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    bool open = false;
    HRESULT error = S_OK;   // first recording error, reported by close() as D3D12 does

    HRESULT begin(CommandAllocator& allocator);
};

struct CommandQueue::Impl {
    Device::Impl* device = nullptr;
    QueueSlot* slot = nullptr;
    D3D12_COMMAND_QUEUE_DESC desc{};
    std::vector<VkSemaphoreSubmitInfo> pending_waits;   // guarded by slot->submit_lock

    HRESULT submit(std::span<const VkCommandBufferSubmitInfo> cmds, const VkSemaphoreSubmitInfo* signal);
};

} // namespace vgd
