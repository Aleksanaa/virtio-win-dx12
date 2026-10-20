// SPDX-License-Identifier: MIT
// Driver objects that live in runtime-allocated storage (the pDrvPrivate of each DDI handle, sized by the matching
// CalcPrivate*Size). Each wraps core objects; shared ownership lets a pipeline outlive the shaders, states and root
// signature the runtime destroys right after creating it.
#pragma once

#include "wdk.h"

#include <vgd/pipeline.h>
#include <vgd/root_signature.h>

#include <memory>
#include <vector>

namespace vgd::umd {

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

// Shader, state and pipeline DDIs (pipeline_ddi.cpp).
void fill_pipeline_ddi(D3D12DDI_DEVICE_FUNCS_CORE_0088& table);

// Command pools, recorders, lists and queues (command_ddi.cpp).
void fill_command_ddi(D3D12DDI_DEVICE_FUNCS_CORE_0088& table);
void fill_command_list_ddi(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& table);
void fill_command_queue_ddi(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001& table);

} // namespace vgd::umd
