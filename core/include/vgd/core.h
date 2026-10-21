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
    // VK_EXT_mutable_descriptor_type with descriptor indexing: shader-visible descriptor heaps and descriptor tables.
    bool bindless = false;
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

// A depth/stencil view: what a DSV descriptor holds. `aspects` are the planes the image has.
struct DepthStencilView {
    Resource* resource = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspects = 0;
    uint32_t width = 0, height = 0;
    uint32_t mip = 0, first_layer = 0, layers = 1;
    D3D12_DSV_FLAGS flags = D3D12_DSV_FLAG_NONE;   // read-only planes (recorded; not enforced yet)
};

// A descriptor: plain data, kDescriptorStride bytes apart in descriptor heap memory, so the UMD can hand out
// CPU handles that are addresses of these records. Views and samplers it names belong to their resource (or, for
// samplers, to the device), as D3D12 descriptors do not own what they describe.
constexpr uint32_t kDescriptorStride = 64;

// alignas makes each record exactly one stride.
struct alignas(kDescriptorStride) Descriptor {
    enum class Kind : uint32_t {
        none,
        rtv,
        dsv,
        sampled_image,    // texture SRV
        storage_image,    // texture UAV
        uniform_texel,    // typed buffer SRV
        storage_texel,    // typed buffer UAV
        uniform_buffer,   // CBV
        storage_buffer,   // raw or structured buffer SRV/UAV
        sampler,
    };
    Kind kind = Kind::none;
    union {
        RenderTargetView rtv;
        DepthStencilView dsv;
        struct {   // textures; view VK_NULL_HANDLE: a null descriptor
            VkImageView view;
            Resource* resource;
            VkFormat format;
            uint32_t mip, first_layer, layers;   // what a UAV clear covers
        } image;
        struct {   // typed buffers
            VkBufferView view;
            VkBuffer buffer;
            VkDeviceSize offset, range;
            VkFormat format;
        } texel;
        struct {
            VkBuffer buffer;
            VkDeviceSize offset, range;
        } buffer;
        VkSampler sampler;
    };
    Descriptor() : rtv() {}
};
static_assert(sizeof(Descriptor) == kDescriptorStride);

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
class DescriptorHeap;
class QueryHeap;

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
    HRESULT create_depth_stencil_view(Resource& resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
                                      DepthStencilView* out);

    // Descriptor heaps. Shader-visible CBV/SRV/UAV and sampler heaps are backed by a Vulkan descriptor set
    // (needs mutable descriptors: AdapterDesc::bindless).
    HRESULT create_descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count, bool shader_visible,
                                   std::unique_ptr<DescriptorHeap>* out);
    // View and sampler descriptors, returned by value; write_descriptor() stores one. A null `resource` makes a
    // null descriptor of the view's kind.
    HRESULT create_shader_resource_view(Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
                                        Descriptor* out);
    HRESULT create_unordered_access_view(Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,
                                         Descriptor* out);
    HRESULT create_constant_buffer_view(const D3D12_CONSTANT_BUFFER_VIEW_DESC& desc, Descriptor* out);
    HRESULT create_sampler(const D3D12_SAMPLER_DESC& desc, Descriptor* out);
    // Stores `value` at `dst`, a descriptor in any heap of this device, and in the heap's descriptor set if it has
    // one. Safe from any thread, like the D3D12 calls it serves.
    void write_descriptor(Descriptor* dst, const Descriptor& value);
    // CopyDescriptors: `count` descriptors from `src` (a CPU-only heap, as D3D12 requires) to `dst`.
    void copy_descriptors(Descriptor* dst, const Descriptor* src, uint32_t count);
    // Occlusion, timestamp and pipeline statistics heaps (no stream output, copy queue timestamps or mesh shader
    // statistics). Every query starts out available: zero, or for timestamps some time before creation.
    HRESULT create_query_heap(const D3D12_QUERY_HEAP_DESC& desc, std::unique_ptr<QueryHeap>* out);
    // The rate GPU timestamps tick at, per second.
    uint64_t timestamp_frequency() const;
    // The rate ResolveQueryData reports timestamps in, when that is not the GPU's: on Windows the KMD decides what
    // GetTimestampFrequency returns. 0 (the default) keeps the GPU's. Applies to resolves recorded afterwards.
    void set_timestamp_frequency(uint64_t hz);
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

// A D3D12 descriptor heap: kDescriptorStride-spaced records, plus a descriptor set when shader-visible.
class DescriptorHeap {
public:
    ~DescriptorHeap();
    D3D12_DESCRIPTOR_HEAP_TYPE type() const;
    uint32_t count() const;
    bool shader_visible() const;
    Descriptor* cpu_start();
    // Shader-visible heaps: the GPU handle of descriptor 0 (an opaque value; handles step by kDescriptorStride).
    uint64_t gpu_start() const;

    struct Impl;
    Impl& impl() { return *impl_; }
private:
    friend class Device;
    explicit DescriptorHeap(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

class QueryHeap {
public:
    ~QueryHeap();
    D3D12_QUERY_HEAP_TYPE type() const;
    uint32_t count() const;

    struct Impl;
    Impl& impl() { return *impl_; }
private:
    friend class Device;
    explicit QueryHeap(std::unique_ptr<Impl> impl);
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
    // The shader-visible heaps descriptor tables point into (at most one CBV/SRV/UAV heap and one sampler heap).
    void set_descriptor_heaps(std::span<DescriptorHeap* const> heaps);
    // Compute root arguments, separate from the graphics ones as in D3D12.
    void set_compute_root_signature(const RootSignature* rs);
    void set_compute_root_32bit_constants(uint32_t parameter, std::span<const uint32_t> values, uint32_t dest_offset);
    void set_compute_root_descriptor(uint32_t parameter, D3D12_GPU_VIRTUAL_ADDRESS address);
    void set_compute_root_descriptor_table(uint32_t parameter, uint64_t gpu_handle);
    void dispatch(uint32_t x, uint32_t y, uint32_t z);
    // ClearUnorderedAccessView{Uint,Float} on what a UAV descriptor covers. Buffers clear with a fill (raw,
    // structured and 32-bit typed views); rects are not supported yet.
    void clear_unordered_access_view_uint(const Descriptor& uav, const uint32_t values[4]);
    void clear_unordered_access_view_float(const Descriptor& uav, const float values[4]);
    void set_graphics_root_descriptor_table(uint32_t parameter, uint64_t gpu_handle);
    void set_primitive_topology(D3D12_PRIMITIVE_TOPOLOGY topology);
    void set_viewports(std::span<const D3D12_VIEWPORT> viewports);
    void set_scissor_rects(std::span<const D3D12_RECT> rects);
    void set_blend_factor(const float factor[4]);
    void set_stencil_ref(uint32_t ref);
    void set_vertex_buffers(uint32_t start_slot, std::span<const D3D12_VERTEX_BUFFER_VIEW> views);
    void set_index_buffer(const D3D12_INDEX_BUFFER_VIEW* view);
    void set_render_targets(std::span<const RenderTargetView> rtvs, const DepthStencilView* dsv = nullptr);
    void clear_render_target_view(const RenderTargetView& rtv, const float color[4], std::span<const D3D12_RECT> rects);
    void clear_depth_stencil_view(const DepthStencilView& dsv, D3D12_CLEAR_FLAGS flags, float depth, uint8_t stencil,
                                  std::span<const D3D12_RECT> rects);
    void set_depth_bounds(float min, float max);
    // ResolveSubresource: a multisampled color subresource into a single-sampled one.
    void resolve_subresource(Resource& dst, uint32_t dst_subresource, Resource& src, uint32_t src_subresource,
                             DXGI_FORMAT format);
    // Queries. Occlusion and pipeline statistics queries need a direct list, and a list ends the ones still active
    // when it closes. Each begin (and each timestamp) ends the current rendering: Vulkan resets queries outside it.
    void begin_query(QueryHeap& heap, D3D12_QUERY_TYPE type, uint32_t index);
    void end_query(QueryHeap& heap, D3D12_QUERY_TYPE type, uint32_t index);
    // Results as 64-bit values (pipeline statistics: D3D12_QUERY_DATA_PIPELINE_STATISTICS each) at `offset`, a
    // multiple of 8. Binary occlusion results and rescaled timestamps are fixed up by a compute shader, which copy
    // lists cannot run: there timestamps stay in GPU ticks.
    void resolve_query_data(QueryHeap& heap, D3D12_QUERY_TYPE type, uint32_t start, uint32_t count, Resource& dst,
                            uint64_t offset);
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
