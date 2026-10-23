// SPDX-License-Identifier: MIT
// Driver objects that live in runtime-allocated storage (the pDrvPrivate of each DDI handle, sized by the matching
// CalcPrivate*Size). Each holds the vkd3d-proton object the DDI object stands for, or, for the DDI's pieces of what
// the API creates whole (shaders, fixed-function states, input layouts), what the pipeline state needs of it.
#pragma once

#include "com.h"
#include "queue_bridge.h"
#include "shared_image.h"
#include "wdk.h"

#include <memory>
#include <string>
#include <vector>

namespace vgd::umd {

struct Device;

template <class T, class H>
T* object(H handle)
{
    return static_cast<T*>(handle.pDrvPrivate);
}

struct RootSignatureObject {
    Com<ID3D12RootSignature> rs;
};

// A shader as a container vkd3d-proton takes (shader_container.h), with its stream output when it has one.
struct ShaderObject {
    std::vector<uint8_t> container;
    std::vector<D3D12_SO_DECLARATION_ENTRY> so_entries;   // names point into the static generic_semantic() names
    std::vector<UINT> so_strides;
    UINT so_rasterized_stream = 0;
    bool stream_output = false;

    D3D12_SHADER_BYTECODE bytecode() const { return {container.data(), container.size()}; }
};

struct ElementLayoutObject {
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;   // named by register (shader_container.h)
};

struct BlendStateObject {
    D3D12_BLEND_DESC desc;
};

struct RasterizerStateObject {
    D3D12_RASTERIZER_DESC desc;   // vkd3d-proton promotes it as the API does
};

struct DepthStencilStateObject {
    D3D12_DEPTH_STENCIL_DESC2 desc;
};

struct PipelineStateObject {
    Com<ID3D12PipelineState> pso;
};

struct CommandListObject {
    Device* device;
    D3D12DDI_HRTCOMMANDLIST rt;
    D3D12_COMMAND_LIST_TYPE type;
    Com<ID3D12GraphicsCommandList> list;
    // The newest interfaces vkd3d-proton implements, for the commands only they have; null if it lacks them.
    Com<ID3D12GraphicsCommandList1> list1;
    Com<ID3D12GraphicsCommandList2> list2;
    Com<ID3D12GraphicsCommandList5> list5;

    void error(HRESULT hr) const;
};

// CreateHeapAndResource gives the heap and the resource separate storage, in three shapes:
//   - committed (both descriptions): the heap owns the resource and the resource handle refers to it. A buffer is
//     placed at 0 of a heap of its own instead, because CreateHeap arrives in this shape (a buffer spanning the
//     heap) and placed resources then name that heap by the buffer's handle;
//   - a heap alone: the heap owns a heap, and the runtime's resource handle for it names the heap;
//   - placed or reserved (a resource description alone): the resource owns its resource, placed in the heap its
//     ReuseBufferGPUVA names (by that heap's resource handle) at the offset given there, or reserved if none.
struct HeapObject {
    Com<ID3D12Heap> heap;              // before the resources, which may be placed in it
    Com<ID3D12Resource> resource;      // committed, or the buffer at 0 of `heap`
    Com<ID3D12Resource> map_buffer;    // a heap alone: a buffer spanning it, made when it is first mapped
    void* mapped = nullptr;
    // Swapchain buffers (PRIMARY heaps) and opened shared textures: the image on shared memory, and for swapchain
    // buffers the runtime allocation aliasing it (present.cpp).
    std::unique_ptr<SharedImage> shared;
    D3DKMT_HANDLE allocation = 0;
    D3D12DDI_HRTRESOURCE rt_resource{};

    // The resource the heap's resource handle names: the committed one, or the shared image's.
    ID3D12Resource* owned() const { return shared ? shared->resource() : resource.get(); }
};

struct ResourceObject {
    HeapObject* heap = nullptr;        // committed resources and heaps; null for placed and reserved resources
    ID3D12Resource* resource = nullptr;   // null for a heap's resource handle
    Com<ID3D12Resource> placed;        // placed and reserved resources own theirs
    HeapObject* placed_in = nullptr;   // placed resources: their heap, and where in it
    uint64_t offset = 0;
};

// Struct-returning COM methods take the return slot as a parameter in MinGW's view of the headers (the MSVC ABI).
inline D3D12_RESOURCE_DESC desc_of(ID3D12Resource* r)
{
    D3D12_RESOURCE_DESC d;
    r->GetDesc(&d);
    return d;
}

inline D3D12_HEAP_DESC desc_of(ID3D12Heap* h)
{
    D3D12_HEAP_DESC d;
    h->GetDesc(&d);
    return d;
}

inline D3D12_RESOURCE_ALLOCATION_INFO allocation_info(ID3D12Device8* d, const D3D12_RESOURCE_DESC1& desc)
{
    D3D12_RESOURCE_ALLOCATION_INFO a;
    d->GetResourceAllocationInfo2(&a, 0, 1, &desc, nullptr);
    return a;
}

inline ID3D12Resource* resource_of(D3D12DDI_HRESOURCE h)
{
    return h.pDrvPrivate ? object<ResourceObject>(h)->resource : nullptr;
}

// The heap a resource handle names: a heap's own handle, or a committed buffer's (which is a heap with the buffer at
// 0). Null for anything else.
inline ID3D12Heap* heap_of(D3D12DDI_HRESOURCE h)
{
    const ResourceObject* r = h.pDrvPrivate ? object<ResourceObject>(h) : nullptr;
    return r && r->heap ? r->heap->heap.get() : nullptr;
}

struct DescriptorHeapObject {
    Com<ID3D12DescriptorHeap> heap;
};

struct QueryHeapObject {
    Com<ID3D12QueryHeap> heap;
};

struct CommandSignatureObject {
    Com<ID3D12CommandSignature> signature;
};

// The runtime owns D3D12 fences (kernel monitored fences it signals and waits on our queues' contexts); the driver
// only learns their GPU addresses, which it needs only for multi-adapter.
struct FenceObject {
    D3D12DDI_FENCE fence;
};

// Presentable copies of presented resources that have no runtime allocation (present.cpp): two, so a present can
// copy while the one before reads the other.
struct PresentCopies {
    Device* device = nullptr;
    std::unique_ptr<HeapObject> surfaces[2];
    unsigned next = 0;
    ~PresentCopies();
};

// Queues (command_ddi.cpp; present.cpp reads the bridge's context and keeps the present copies).
struct CommandQueueObject {
    Com<ID3D12CommandQueue> queue;
    PresentCopies present_copies;
    std::unique_ptr<QueueBridge> bridge;   // declared last, so destroyed first: its threads use the queue
};

// Swapchain buffers, Present and the DXGI table (present.cpp).
HRESULT create_primary(Device& d, const D3D12DDIARG_CREATEHEAP_0001& heap, const D3D12DDIARG_CREATERESOURCE_0088& args,
                       const D3D12_RESOURCE_DESC& desc, D3D12DDI_HRTRESOURCE rt_resource, HeapObject& out);
void destroy_primary(Device& d, HeapObject& heap);
// OpenHeapAndResource of a texture yttrium's D3D10/11 UMD shares: `out` gets an imported texture.
HRESULT open_shared(Device& d, const D3D12DDIARG_OPENHEAP_0003& args, HeapObject& out);
void fill_present_ddi(D3D12DDI_DEVICE_FUNCS_CORE_0088& table);
void fill_present_list_ddi(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& table);
void fill_dxgi_ddi(DXGI1_4_DDI_BASE_FUNCTIONS& table);

// Descriptor heaps and views (descriptor_ddi.cpp).
void fill_descriptor_ddi(D3D12DDI_DEVICE_FUNCS_CORE_0088& table);

// Heaps, resources, mapping, fences and tiles (resource_ddi.cpp).
void fill_resource_ddi(D3D12DDI_DEVICE_FUNCS_CORE_0088& table);
void fill_resource_queue_ddi(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001& table);
// The API's description of a DDI resource description.
D3D12_RESOURCE_DESC1 resource_desc(const D3D12DDIARG_CREATERESOURCE_0088& r);

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
