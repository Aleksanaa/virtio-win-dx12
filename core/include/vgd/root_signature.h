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
// Root arguments live in one block of "root data" that becomes Vulkan push constants, laid out the way dxil-spirv
// expects: first every root CBV/SRV/UAV descriptor as an 8-byte GPU virtual address, in parameter order, then
// 32-bit words for root constants (one per value) and descriptor tables (an offset, in descriptors, into the bound
// heap), again in parameter order.
class RootSignature {
public:
    struct Parameter {
        D3D12_ROOT_PARAMETER_TYPE type;
        uint32_t offset;   // bytes into the root data
        uint32_t size;     // bytes
        // Root descriptors: the Nth root descriptor. Constants and tables: the first word in the 32-bit word area.
        uint32_t index;
        // Descriptor tables: the table points into the sampler heap (its ranges are samplers), not the CBV/SRV/UAV heap.
        bool sampler_table;
    };

    static HRESULT create(const D3D12_ROOT_SIGNATURE_DESC1& desc, std::unique_ptr<RootSignature>* out);

    const D3D12_ROOT_SIGNATURE_DESC1& desc() const { return desc_; }
    std::span<const Parameter> parameters() const { return params_; }
    uint32_t root_data_size() const { return root_data_size_; }
    uint32_t root_descriptor_count() const { return root_descriptors_; }
    uint32_t root_word_count() const { return root_words_; }

private:
    RootSignature() = default;

    D3D12_ROOT_SIGNATURE_DESC1 desc_{};
    std::vector<D3D12_ROOT_PARAMETER1> root_params_;
    std::vector<D3D12_DESCRIPTOR_RANGE1> ranges_;          // all tables' ranges, back to back
    std::vector<D3D12_STATIC_SAMPLER_DESC> static_samplers_;
    std::vector<Parameter> params_;
    uint32_t root_data_size_ = 0;
    uint32_t root_descriptors_ = 0;
    uint32_t root_words_ = 0;
};

} // namespace vgd
