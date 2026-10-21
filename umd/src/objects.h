// SPDX-License-Identifier: MIT
// Driver objects that live in runtime-allocated storage (the pDrvPrivate of each DDI handle, sized by the matching
// CalcPrivate*Size). Each wraps core objects; shared ownership lets a pipeline outlive the shaders, states and root
// signature the runtime destroys right after creating it.
#pragma once

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

// CreateHeapAndResource gives the heap and the resource separate storage. A committed resource is both at once: the
// heap owns the core resource and the resource handle refers to it. Placed resources come later.
struct HeapObject {
    std::unique_ptr<vgd::Resource> resource;
};

struct ResourceObject {
    HeapObject* heap;
    vgd::Resource* resource;
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

// The runtime owns D3D12 fences (kernel monitored fences it signals and waits on our queues' contexts); the driver
// only learns their GPU addresses, which it needs only for multi-adapter.
struct FenceObject {
    D3D12DDI_FENCE fence;
};

// Descriptor heaps and views (descriptor_ddi.cpp).
void fill_descriptor_ddi(D3D12DDI_DEVICE_FUNCS_CORE_0088& table);

// Heaps, resources, mapping and fences (resource_ddi.cpp).
void fill_resource_ddi(D3D12DDI_DEVICE_FUNCS_CORE_0088& table);

// Shader, state and pipeline DDIs (pipeline_ddi.cpp).
void fill_pipeline_ddi(D3D12DDI_DEVICE_FUNCS_CORE_0088& table);

// Command pools, recorders, lists and queues (command_ddi.cpp).
void fill_command_ddi(D3D12DDI_DEVICE_FUNCS_CORE_0088& table);
void fill_command_list_ddi(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& table);
void fill_command_queue_ddi(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001& table);

} // namespace vgd::umd
