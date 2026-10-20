// SPDX-License-Identifier: MIT
#pragma once

#include <vgd/d3d12.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace vgd {

// A D3D12 root signature (version 1.1), deep-copied, with the layout of its root arguments.
//
// Root arguments live in one block of "root data" that becomes Vulkan push data: root constants take 4 bytes each,
// root CBV/SRV/UAV descriptors an 8-byte GPU virtual address, descriptor tables a 4-byte offset (in descriptors) into
// the bound descriptor heap. Pipelines map shader registers onto this layout.
class RootSignature {
public:
    struct Parameter {
        D3D12_ROOT_PARAMETER_TYPE type;
        uint32_t offset;   // bytes into the root data
        uint32_t size;     // bytes
    };

    static HRESULT create(const D3D12_ROOT_SIGNATURE_DESC1& desc, std::unique_ptr<RootSignature>* out);

    const D3D12_ROOT_SIGNATURE_DESC1& desc() const { return desc_; }
    std::span<const Parameter> parameters() const { return params_; }
    uint32_t root_data_size() const { return root_data_size_; }

private:
    RootSignature() = default;

    D3D12_ROOT_SIGNATURE_DESC1 desc_{};
    std::vector<D3D12_ROOT_PARAMETER1> root_params_;
    std::vector<D3D12_DESCRIPTOR_RANGE1> ranges_;          // all tables' ranges, back to back
    std::vector<D3D12_STATIC_SAMPLER_DESC> static_samplers_;
    std::vector<Parameter> params_;
    uint32_t root_data_size_ = 0;
};

} // namespace vgd
