// SPDX-License-Identifier: MIT
// Platform-neutral D3D12 semantics on Vulkan. The Windows UMD and the Linux tests both drive this API;
// neither sees Vulkan handles directly.
#pragma once

#include <vgd/d3d12.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace vgd {

enum class LogLevel { error, warn, info, debug };
using LogSink = void (*)(LogLevel level, const char* message);
void set_log_sink(LogSink sink);

struct AdapterDesc {
    std::string name;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint32_t api_version = 0;
    std::string driver_info;
    uint64_t device_local_bytes = 0;
    uint64_t host_visible_bytes = 0;
    // VK_EXT_descriptor_heap. Required for descriptors and root signatures, but not for bring-up of devices,
    // memory, copies and fences, so it is reported rather than required (Venus needs virglrenderer >= 2a671dbf).
    bool descriptor_heap = false;
    // Features this driver requires but the Vulkan device lacks. Empty means the adapter is usable.
    std::vector<std::string> missing;
    bool usable() const { return missing.empty(); }
};

class Device;
class Resource;
class CommandAllocator;
class CommandList;
class CommandQueue;
class Fence;

class Instance {
public:
    // `gipa` is the loader's vkGetInstanceProcAddr on Linux, or the ICD's vk_icdGetInstanceProcAddr on Windows.
    static HRESULT create(PFN_vkGetInstanceProcAddr gipa, std::unique_ptr<Instance>* out);
    ~Instance();

    const std::vector<AdapterDesc>& adapters() const;
    HRESULT create_device(size_t adapter_index, std::unique_ptr<Device>* out);

    struct Impl;
private:
    explicit Instance(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

class Device {
public:
    ~Device();
    const AdapterDesc& adapter() const;

    // Committed buffer: its own heap of the given type, like ID3D12Device::CreateCommittedResource with a buffer desc.
    HRESULT create_committed_buffer(D3D12_HEAP_TYPE heap_type, uint64_t size, D3D12_RESOURCE_FLAGS flags,
                                    std::unique_ptr<Resource>* out);
    HRESULT create_fence(uint64_t initial_value, std::unique_ptr<Fence>* out);
    HRESULT create_command_queue(const D3D12_COMMAND_QUEUE_DESC& desc, std::unique_ptr<CommandQueue>* out);
    HRESULT create_command_allocator(D3D12_COMMAND_LIST_TYPE type, std::unique_ptr<CommandAllocator>* out);
    // The list starts open, recording into `allocator`, as in D3D12.
    HRESULT create_command_list(D3D12_COMMAND_LIST_TYPE type, CommandAllocator& allocator,
                                std::unique_ptr<CommandList>* out);

    struct Impl;
    Impl& impl() { return *impl_; }
private:
    friend class Instance;
    explicit Device(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

class Resource {
public:
    ~Resource();
    uint64_t size() const;
    D3D12_GPU_VIRTUAL_ADDRESS gpu_virtual_address() const;
    // Persistent CPU pointer for UPLOAD/READBACK heaps; nullptr for DEFAULT.
    void* cpu_pointer() const;

    struct Impl;
    Impl& impl() { return *impl_; }
private:
    friend class Device;
    explicit Resource(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

class Fence {
public:
    ~Fence();
    uint64_t completed_value() const;
    HRESULT signal(uint64_t value);                         // CPU signal
    HRESULT wait(uint64_t value, uint64_t timeout_ns);      // CPU wait; S_FALSE on timeout

    struct Impl;
    Impl& impl() { return *impl_; }
private:
    friend class Device;
    explicit Fence(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

class CommandAllocator {
public:
    ~CommandAllocator();
    // The caller guarantees the GPU has finished with every list recorded from this allocator.
    HRESULT reset();

    struct Impl;
    Impl& impl() { return *impl_; }
private:
    friend class Device;
    explicit CommandAllocator(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

class CommandList {
public:
    ~CommandList();
    HRESULT close();
    HRESULT reset(CommandAllocator& allocator);

    void copy_buffer_region(Resource& dst, uint64_t dst_offset, Resource& src, uint64_t src_offset, uint64_t bytes);
    // Legacy D3D12 transition barriers on buffers become one global memory dependency.
    void resource_barrier(std::span<const D3D12_RESOURCE_BARRIER> barriers);

    struct Impl;
    Impl& impl() { return *impl_; }
private:
    friend class Device;
    explicit CommandList(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

class CommandQueue {
public:
    ~CommandQueue();
    HRESULT execute_command_lists(std::span<CommandList* const> lists);
    HRESULT signal(Fence& fence, uint64_t value);
    HRESULT wait(Fence& fence, uint64_t value);

    struct Impl;
    Impl& impl() { return *impl_; }
private:
    friend class Device;
    explicit CommandQueue(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace vgd
