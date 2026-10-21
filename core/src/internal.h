// SPDX-License-Identifier: MIT
#pragma once

#include <vgd/core.h>

#include <vgd/pipeline.h>

#include <array>
#include <atomic>
#include <cstdarg>
#include <functional>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>

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
    VkPhysicalDeviceFeatures features{};   // Vulkan 1.0 features, all enabled except robustBufferAccess
    VkPhysicalDeviceLimits limits{};
    // Vulkan 1.1-1.3 features: everything supported is enabled (pNext is rebuilt at device creation).
    VkPhysicalDeviceVulkan11Features v11{};
    VkPhysicalDeviceVulkan12Features v12{};
    VkPhysicalDeviceVulkan13Features v13{};
    bool mutable_descriptors = false;   // VK_EXT_mutable_descriptor_type (or the VALVE original)
    const char* mutable_ext_name = nullptr;
    bool null_descriptor = false;       // VK_EXT_robustness2 nullDescriptor
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

    // Buffers by GPU virtual address, for the D3D12 commands that take addresses where Vulkan wants buffers
    // (vertex and index buffers).
    std::shared_mutex va_lock;
    std::map<VkDeviceAddress, Resource::Impl*> buffers_by_va;
    void register_buffer(Resource::Impl* r);
    void unregister_buffer(Resource::Impl* r);
    // The buffer containing `va`, with the offset of `va` in it and the bytes from there to its end;
    // VK_NULL_HANDLE if none.
    VkBuffer find_buffer(VkDeviceAddress va, VkDeviceSize* offset, VkDeviceSize* size_left);

    // New images go from UNDEFINED to GENERAL in a small submission of their own on the direct queue. Every queue
    // waits for the latest one before its next submission (CommandQueue::Impl::submit).
    std::mutex init_lock;
    VkCommandPool init_pool = VK_NULL_HANDLE;
    VkSemaphore init_timeline = VK_NULL_HANDLE;
    std::atomic<uint64_t> init_value{0};
    std::vector<std::pair<VkCommandBuffer, uint64_t>> init_cmds;   // reused once their value has passed
    HRESULT create_init_objects();
    HRESULT init_image(VkImage image, VkImageAspectFlags aspects);
    // Records `record` into an initialization command buffer and submits it the same way (query heaps use it too).
    HRESULT init_submit(const std::function<void(VkCommandBuffer)>& record);

    // Bindless descriptors (null layouts without mutable descriptors). Every pipeline layout starts with these
    // two sets: the CBV/SRV/UAV heap at set 0 and the sampler heap at set 1, one binding each, sized per heap.
    VkDescriptorSetLayout resource_set_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout sampler_set_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout empty_set_layout = VK_NULL_HANDLE;   // stands in for the heap sets without bindless
    uint32_t max_resource_descriptors = 0, max_sampler_descriptors = 0;
    HRESULT create_bindless_layouts();

    // Samplers by description; D3D12 sampler descriptors do not own them, so they live as long as the device.
    std::mutex samplers_lock;
    struct SamplerKey {
        uint8_t fields[sizeof(VkSamplerCreateInfo) - offsetof(VkSamplerCreateInfo, flags)];
        VkSamplerReductionMode reduction;
    };
    std::vector<std::pair<SamplerKey, VkSampler>> samplers;
    VkSampler get_sampler(const D3D12_SAMPLER_DESC& desc);

    // Descriptor heaps by the address range of their records, for write_descriptor().
    std::shared_mutex heaps_lock;
    std::map<uintptr_t, DescriptorHeap::Impl*> heaps_by_address;
    DescriptorHeap::Impl* find_heap(const Descriptor* d);

    QueueSlot& slot_for(D3D12_COMMAND_LIST_TYPE type);
    uint32_t find_memory_type(uint32_t type_bits, D3D12_HEAP_TYPE heap_type) const;
    // The Vulkan format this device stores a DXGI format as: vk_format() plus device-specific substitutions
    // (D24S8 becomes D32S8 where the GPU lacks it, e.g. AMD).
    VkFormat resolve_format(DXGI_FORMAT format) const;
    bool d24s8_supported = false;

    Device* self = nullptr;   // the API object, for building internal pipelines with the public calls
    // Queries (queries.cpp): the rate resolved timestamps are reported in (0: the GPU's), and the compute pipeline
    // that fixes up resolved results, built on first use.
    std::atomic<uint64_t> timestamp_target{0};
    std::mutex internal_lock;
    std::unique_ptr<PipelineState> query_fixup;
    const PipelineState* get_query_fixup();
};

// A view of an image, keyed by everything that distinguishes the Vulkan image views we create.
struct ImageViewKey {
    VkImageViewType type;
    VkFormat format;
    VkImageAspectFlags aspects;
    uint32_t mip, mips, layer, layers;
    uint32_t swizzle;            // VkComponentSwizzle r | g << 8 | b << 16 | a << 24 (0: identity)
    VkImageUsageFlags usage;     // what the view is for (VkImageViewUsageCreateInfo)
    bool operator==(const ImageViewKey&) const = default;
};

struct Resource::Impl {
    Device::Impl* device = nullptr;
    D3D12_RESOURCE_DESC desc{};
    VkBuffer buffer = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspects = 0;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint64_t size = 0;
    VkDeviceAddress address = 0;
    void* mapped = nullptr;
    std::mutex views_lock;
    std::vector<std::pair<ImageViewKey, VkImageView>> views;
    struct BufferViewKey {
        VkFormat format;
        VkDeviceSize offset, range;
        bool operator==(const BufferViewKey&) const = default;
    };
    std::vector<std::pair<BufferViewKey, VkBufferView>> buffer_views;
    ~Impl();

    // A cached view; VK_NULL_HANDLE on failure.
    VkImageView view(const ImageViewKey& key);
    VkBufferView buffer_view(const BufferViewKey& key);
    uint32_t array_layers() const
    {
        return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize;
    }
};

struct QueryHeap::Impl {
    Device::Impl* device = nullptr;
    D3D12_QUERY_HEAP_TYPE type{};
    uint32_t count = 0;
    VkQueryPool pool = VK_NULL_HANDLE;
    uint32_t stride = 8;   // bytes per result
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

struct PipelineState::Impl {
    Device::Impl* device = nullptr;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineBindPoint bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
    uint32_t push_size = 0;               // the root signature's root data
    uint32_t vertex_binding_mask = 0;     // input slots the vertex input state declares
    bool primitive_restart = false;       // a strip cut value is set
    // Static samplers: set 2, one immutable sampler per binding, with a set of its own to bind.
    VkDescriptorSetLayout static_layout = VK_NULL_HANDLE;
    VkDescriptorPool static_pool = VK_NULL_HANDLE;
    VkDescriptorSet static_set = VK_NULL_HANDLE;
    HRESULT create_static_samplers(const RootSignature& rs);
    ~Impl();
};

// Converts one shader to SPIR-V, mapping its root parameters onto `rs`'s root data (shader.cpp).
// Descriptor set numbers in every pipeline layout.
constexpr uint32_t kResourceHeapSet = 0, kSamplerHeapSet = 1, kStaticSamplerSet = 2;

struct ShaderOptions {
    uint32_t ssbo_alignment = 16;            // minStorageBufferOffsetAlignment
    bool typed_uav_read_without_format = false;
    bool bindless = false;                   // descriptor tables map to the heap sets
};
HRESULT translate_shader(const Shader& shader, const RootSignature& rs, const ShaderOptions& options,
                         std::vector<uint32_t>* spirv, std::string* entry_point);

// The descriptor type a descriptor kind is written as.
VkDescriptorType vk_descriptor_type(Descriptor::Kind kind);

struct DescriptorHeap::Impl {
    Device::Impl* device = nullptr;
    D3D12_DESCRIPTOR_HEAP_TYPE type{};
    uint32_t count = 0;
    std::unique_ptr<Descriptor[]> records;
    VkDescriptorPool pool = VK_NULL_HANDLE;   // shader-visible heaps only
    VkDescriptorSet set = VK_NULL_HANDLE;
    std::mutex set_lock;                       // vkUpdateDescriptorSets needs the set externally synchronized
    ~Impl();
    // Writes records [first, first + n) to the descriptor set (a no-op without one).
    void flush(uint32_t first, uint32_t n);
};

// The Vulkan stage of a D3D12 shader stage.
VkShaderStageFlagBits vk_stage(ShaderStage stage);

struct CommandList::Impl {
    Device::Impl* device = nullptr;
    D3D12_COMMAND_LIST_TYPE type{};
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    bool open = false;
    HRESULT error = S_OK;   // first recording error, reported by close() as D3D12 does

    // Graphics state, applied lazily at draws.
    const PipelineState* pso = nullptr;
    DescriptorHeap::Impl* resource_heap = nullptr;   // shader-visible heaps from SetDescriptorHeaps
    DescriptorHeap::Impl* sampler_heap = nullptr;
    // D3D12 keeps graphics and compute root signatures and arguments apart, as Vulkan does its bind points.
    struct RootState {
        const RootSignature* rs = nullptr;
        std::array<uint8_t, 256> data{};
        bool dirty = false;                   // root data changed since the last push
        bool heaps_dirty = false;             // heap sets need binding
        const PipelineState* bound = nullptr; // the pipeline bound at this bind point
    };
    RootState graphics_root, compute_root;
    // Binds `p` if new, then its descriptor sets and push constants as needed.
    void flush_root(RootState& root, VkPipelineBindPoint bind_point, const PipelineState* p);
    bool prepare_dispatch();
    D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    std::vector<VkViewport> viewports;
    std::vector<VkRect2D> scissors;
    bool viewports_dirty = true;
    struct VertexBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0, size = 0, stride = 0;
    };
    std::array<VertexBuffer, D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vertex_buffers{};
    bool vertex_buffers_dirty = true;
    VkBuffer index_buffer = VK_NULL_HANDLE;
    VkDeviceSize index_offset = 0;
    VkIndexType index_type = VK_INDEX_TYPE_UINT16;
    bool index_buffer_dirty = false;
    std::array<RenderTargetView, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> rtvs{};
    uint32_t num_rtvs = 0;
    DepthStencilView dsv{};   // dsv.view is VK_NULL_HANDLE when none is bound
    bool rendering = false;
    const PipelineState* rendering_pso = nullptr;   // the pipeline whose attachments the rendering has
    std::vector<std::pair<VkQueryPool, uint32_t>> active_queries;   // begun and not yet ended
    void end_active_queries();

    HRESULT begin(CommandAllocator& allocator);
    void reset_state();
    void fail(HRESULT hr);
    void end_rendering();
    // Flushes state for a draw; false if the draw has to be dropped.
    bool prepare_draw();
};

struct CommandQueue::Impl {
    Device::Impl* device = nullptr;
    QueueSlot* slot = nullptr;
    D3D12_COMMAND_QUEUE_DESC desc{};
    std::vector<VkSemaphoreSubmitInfo> pending_waits;   // guarded by slot->submit_lock
    uint64_t init_waited = 0;                            // last image initialization this queue waited for

    HRESULT submit(std::span<const VkCommandBufferSubmitInfo> cmds, const VkSemaphoreSubmitInfo* signal);
};

} // namespace vgd
