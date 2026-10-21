// SPDX-License-Identifier: MIT
// Platform-neutral D3D12 semantics on Vulkan. The Windows UMD and the Linux tests both drive this API;
// neither sees Vulkan handles directly.
#pragma once

#include <vgd/d3d12.h>
#include <vgd/formats.h>

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
class PipelineState;
class RootSignature;

// A render target view: what an RTV descriptor holds. Plain data, so the UMD can store it in descriptor memory; the
// Vulkan view belongs to the resource and lives as long as it does.
struct RenderTargetView {
    Resource* resource = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    uint32_t mip = 0, first_layer = 0, layers = 1;
};

// One side of CopyTextureRegion: a texture subresource, or a placed footprint in a buffer.
struct TextureCopyLocation {
    Resource* resource = nullptr;
    bool is_footprint = false;
    uint32_t subresource = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
};

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
    // Vulkan features of the format a DXGI format is stored as (all zero if we do not map it).
    FormatFeatures format_features(DXGI_FORMAT format) const;

    // Committed buffer: its own heap of the given type, like ID3D12Device::CreateCommittedResource with a buffer desc.
    HRESULT create_committed_buffer(D3D12_HEAP_TYPE heap_type, uint64_t size, D3D12_RESOURCE_FLAGS flags,
                                    std::unique_ptr<Resource>* out);
    // Committed resource of any dimension. Textures live in DEFAULT heaps only.
    HRESULT create_committed_resource(D3D12_HEAP_TYPE heap_type, const D3D12_RESOURCE_DESC& desc,
                                      std::unique_ptr<Resource>* out);
    // What a texture needs from a heap (GetResourceAllocationInfo); buffers need their size at 64 KiB alignment.
    HRESULT resource_allocation_info(const D3D12_RESOURCE_DESC& desc, uint64_t* size, uint64_t* alignment) const;
    // A view of a texture for OMSetRenderTargets / ClearRenderTargetView. `desc` may be null (whole mip 0).
    HRESULT create_render_target_view(Resource& resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc,
                                      RenderTargetView* out);
    HRESULT create_fence(uint64_t initial_value, std::unique_ptr<Fence>* out);
    HRESULT create_command_queue(const D3D12_COMMAND_QUEUE_DESC& desc, std::unique_ptr<CommandQueue>* out);
    HRESULT create_command_allocator(D3D12_COMMAND_LIST_TYPE type, std::unique_ptr<CommandAllocator>* out);
    // The list starts open, recording into `allocator`, as in D3D12.
    HRESULT create_command_list(D3D12_COMMAND_LIST_TYPE type, CommandAllocator& allocator,
                                std::unique_ptr<CommandList>* out);
    // A closed list with no allocator yet; reset() opens it (the DDI creates lists before giving them a pool).
    HRESULT create_command_list(D3D12_COMMAND_LIST_TYPE type, std::unique_ptr<CommandList>* out);

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
    const D3D12_RESOURCE_DESC& desc() const;
    bool is_texture() const { return desc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER; }
    // Buffers: their width. Textures: the memory they occupy.
    uint64_t size() const;
    // Buffers only (0 for textures).
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
    // Texture <-> footprint or texture <-> texture. `src_box` (null: the whole source) is in texels.
    void copy_texture_region(const TextureCopyLocation& dst, uint32_t x, uint32_t y, uint32_t z,
                             const TextureCopyLocation& src, const D3D12_BOX* src_box);
    // Whole resources of the same shape.
    void copy_resource(Resource& dst, Resource& src);
    // Legacy D3D12 barriers become one global memory dependency: textures stay in VK_IMAGE_LAYOUT_GENERAL.
    void resource_barrier(std::span<const D3D12_RESOURCE_BARRIER> barriers);
    // A full memory dependency between everything recorded before and after.
    void global_barrier();

    // Graphics state. Like D3D12, everything is reset by reset().
    void set_pipeline_state(const PipelineState* pso);
    void set_graphics_root_signature(const RootSignature* rs);
    void set_graphics_root_32bit_constants(uint32_t parameter, std::span<const uint32_t> values, uint32_t dest_offset);
    // Root CBV/SRV/UAV.
    void set_graphics_root_descriptor(uint32_t parameter, D3D12_GPU_VIRTUAL_ADDRESS address);
    void set_primitive_topology(D3D12_PRIMITIVE_TOPOLOGY topology);
    void set_viewports(std::span<const D3D12_VIEWPORT> viewports);
    void set_scissor_rects(std::span<const D3D12_RECT> rects);
    void set_blend_factor(const float factor[4]);
    void set_stencil_ref(uint32_t ref);
    void set_vertex_buffers(uint32_t start_slot, std::span<const D3D12_VERTEX_BUFFER_VIEW> views);
    void set_index_buffer(const D3D12_INDEX_BUFFER_VIEW* view);
    void set_render_targets(std::span<const RenderTargetView> rtvs);
    void clear_render_target_view(const RenderTargetView& rtv, const float color[4], std::span<const D3D12_RECT> rects);
    void draw_instanced(uint32_t vertex_count, uint32_t instance_count, uint32_t start_vertex, uint32_t start_instance);
    void draw_indexed_instanced(uint32_t index_count, uint32_t instance_count, uint32_t start_index,
                                int32_t base_vertex, uint32_t start_instance);

    struct Impl;
    Impl& impl() { return *impl_; }
private:
    friend class Device;
    explicit CommandList(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// What closed command lists recorded, captured at ExecuteCommandLists time. D3D12 lets an application reset a list
// right after executing it, so work that is submitted later (by the UMD's queue thread) must not read the lists.
struct CommandBatch {
    std::vector<VkCommandBuffer> buffers;
};

class CommandQueue {
public:
    ~CommandQueue();
    // Fails if a list is still open.
    static HRESULT capture(std::span<CommandList* const> lists, CommandBatch* out);
    HRESULT submit(const CommandBatch& batch, Fence* signal = nullptr, uint64_t value = 0);
    // With `signal`, the same submission also signals that fence to `value` when the lists complete.
    HRESULT execute_command_lists(std::span<CommandList* const> lists, Fence* signal = nullptr, uint64_t value = 0);
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
