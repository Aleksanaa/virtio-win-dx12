// SPDX-License-Identifier: MIT
// Driver objects that live in runtime-allocated storage (the pDrvPrivate of each DDI handle, sized by the matching
// CalcPrivate*Size). Each wraps core objects; shared ownership lets a pipeline outlive the shaders, states and root
// signature the runtime destroys right after creating it.
#pragma once

#include "queue_bridge.h"
#include "wdk.h"

#include <vgd/core.h>
#include <vgd/pipeline.h>
#include <vgd/root_signature.h>

#include <memory>
#include <vector>

namespace vgd::umd {

struct Device;

template <class T, class H>
T* object(H handle)
{
    return static_cast<T*>(handle.pDrvPrivate);
}

struct RootSignatureObject {
    std::shared_ptr<const vgd::RootSignature> rs;
};

struct ShaderObject {
    std::shared_ptr<const vgd::Shader> shader;
};

struct ElementLayoutObject {
    std::vector<vgd::VertexElement> elements;
};

struct BlendStateObject {
    D3D12_BLEND_DESC desc;
};

struct RasterizerStateObject {
    D3D12_RASTERIZER_DESC desc;
};

struct DepthStencilStateObject {
    D3D12_DEPTH_STENCIL_DESC1 desc;
};

struct PipelineStateObject {
    std::unique_ptr<vgd::PipelineState> pso;
};

struct CommandListObject {
    Device* device;
    D3D12DDI_HRTCOMMANDLIST rt;
    D3D12_COMMAND_LIST_TYPE type;
    std::unique_ptr<vgd::CommandList> list;

    void error(HRESULT hr) const;
};

// CreateHeapAndResource gives the heap and the resource separate storage, in three shapes:
//   - committed (both descriptions): the heap owns the core resource and the resource handle refers to it. For a
//     buffer the heap also owns a core heap the resource is placed in at 0: CreateHeap arrives in this shape;
//   - a heap alone: the heap owns a core heap, and the runtime's resource handle for it names the heap;
//   - placed (a resource description alone): the resource owns a core resource in the heap its
//     ReuseBufferGPUVA names (by that heap's resource handle) at the offset given there.
struct HeapObject {
    std::unique_ptr<vgd::Heap> heap;            // before the resource, which may be placed in it
    std::unique_ptr<vgd::Resource> resource;
    // Swapchain buffers (PRIMARY heaps) also own a runtime allocation sharing the resource's memory (present.cpp).
    D3DKMT_HANDLE allocation = 0;
    D3D12DDI_HRTRESOURCE rt_resource{};
};

struct ResourceObject {
    HeapObject* heap = nullptr;          // committed resources and heaps; null for placed resources
    vgd::Resource* resource = nullptr;   // null for a heap's resource handle
    std::unique_ptr<vgd::Resource> placed;
    HeapObject* placed_in = nullptr;     // placed resources: their heap, and where in it
    uint64_t offset = 0;
};

// Descriptors are core records (vgd::Descriptor) in core descriptor heaps, kDescriptorSize bytes apart. The runtime
// hands out CPU handles that are plain addresses of those records.
constexpr UINT kDescriptorSize = vgd::kDescriptorStride;

inline vgd::Descriptor* descriptor_at(D3D12DDI_CPU_DESCRIPTOR_HANDLE h)
{
    return reinterpret_cast<vgd::Descriptor*>(h.ptr);
}

struct DescriptorHeapObject {
    std::unique_ptr<vgd::DescriptorHeap> heap;
};

struct QueryHeapObject {
    std::unique_ptr<vgd::QueryHeap> heap;
};

struct CommandSignatureObject {
    vgd::CommandSignature signature;
};

// The runtime owns D3D12 fences (kernel monitored fences it signals and waits on our queues' contexts); the driver
// only learns their GPU addresses, which it needs only for multi-adapter.
struct FenceObject {
    D3D12DDI_FENCE fence;
};

// Queues (command_ddi.cpp; present.cpp reads the bridge's context).
struct CommandQueueObject {
    std::unique_ptr<vgd::CommandQueue> queue;
    std::unique_ptr<QueueBridge> bridge;   // declared last, so destroyed first: its threads use the queue
};

// Swapchain buffers, Present and the DXGI table (present.cpp).
HRESULT create_primary(Device& d, const D3D12DDIARG_CREATEHEAP_0001& heap, const D3D12DDIARG_CREATERESOURCE_0088& args,
                       const D3D12_RESOURCE_DESC& desc, D3D12DDI_HRTRESOURCE rt_resource, HeapObject& out);
void destroy_primary(Device& d, HeapObject& heap);
// OpenHeapAndResource of a texture yttrium's D3D10/11 UMD shares: `out` gets an imported core texture.
HRESULT open_shared(Device& d, const D3D12DDIARG_OPENHEAP_0003& args, HeapObject& out);
void fill_present_ddi(D3D12DDI_DEVICE_FUNCS_CORE_0088& table);
void fill_present_list_ddi(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& table);
void fill_dxgi_ddi(DXGI1_4_DDI_BASE_FUNCTIONS& table);

// Descriptor heaps and views (descriptor_ddi.cpp).
void fill_descriptor_ddi(D3D12DDI_DEVICE_FUNCS_CORE_0088& table);

// Heaps, resources, mapping and fences (resource_ddi.cpp).
void fill_resource_ddi(D3D12DDI_DEVICE_FUNCS_CORE_0088& table);

// Query heaps and queries (query_ddi.cpp).
void fill_query_ddi(D3D12DDI_DEVICE_FUNCS_CORE_0088& table);
void fill_query_list_ddi(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& table);

// Command signatures and ExecuteIndirect (indirect_ddi.cpp).
void fill_indirect_ddi(D3D12DDI_DEVICE_FUNCS_CORE_0088& table);
void fill_indirect_list_ddi(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& table);

// Shader, state and pipeline DDIs (pipeline_ddi.cpp).
void fill_pipeline_ddi(D3D12DDI_DEVICE_FUNCS_CORE_0088& table);

// Command pools, recorders, lists and queues (command_ddi.cpp).
void fill_command_ddi(D3D12DDI_DEVICE_FUNCS_CORE_0088& table);
void fill_command_list_ddi(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& table);
void fill_command_queue_ddi(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001& table);

} // namespace vgd::umd
