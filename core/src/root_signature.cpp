// SPDX-License-Identifier: MIT
#include <vgd/root_signature.h>

#include "internal.h"

namespace vgd {

HRESULT RootSignature::create(const D3D12_ROOT_SIGNATURE_DESC1& desc, std::unique_ptr<RootSignature>* out)
{
    if (!out || (desc.NumParameters && !desc.pParameters) || (desc.NumStaticSamplers && !desc.pStaticSamplers))
        return E_INVALIDARG;
    std::unique_ptr<RootSignature> rs(new RootSignature);

    // D3D12 limits a root signature to 64 DWORDs: a constant costs 1, a root descriptor 2, a table 1.
    uint32_t cost = 0;
    size_t total_ranges = 0;
    for (uint32_t i = 0; i < desc.NumParameters; i++) {
        const auto& p = desc.pParameters[i];
        switch (p.ParameterType) {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
            if (p.DescriptorTable.NumDescriptorRanges && !p.DescriptorTable.pDescriptorRanges)
                return E_INVALIDARG;
            total_ranges += p.DescriptorTable.NumDescriptorRanges;
            cost += 1;
            break;
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS: cost += p.Constants.Num32BitValues; break;
        case D3D12_ROOT_PARAMETER_TYPE_CBV:
        case D3D12_ROOT_PARAMETER_TYPE_SRV:
        case D3D12_ROOT_PARAMETER_TYPE_UAV: cost += 2; break;
        default: return E_INVALIDARG;
        }
    }
    if (cost > 64) {
        log(LogLevel::warn, "root signature costs %u DWORDs, limit 64", cost);
        return E_INVALIDARG;
    }

    // Deep copy: ranges first, so table pointers can point into our own storage.
    rs->ranges_.reserve(total_ranges);
    rs->root_params_.assign(desc.pParameters, desc.pParameters + desc.NumParameters);
    for (auto& p : rs->root_params_) {
        if (p.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
            continue;
        const auto* src = p.DescriptorTable.pDescriptorRanges;
        p.DescriptorTable.pDescriptorRanges = rs->ranges_.data() + rs->ranges_.size();
        rs->ranges_.insert(rs->ranges_.end(), src, src + p.DescriptorTable.NumDescriptorRanges);
        // Resolve D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND, so every range has its offset from the table start.
        uint32_t next = 0;
        for (size_t i = rs->ranges_.size() - p.DescriptorTable.NumDescriptorRanges; i < rs->ranges_.size(); i++) {
            auto& r = rs->ranges_[i];
            if (r.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND)
                r.OffsetInDescriptorsFromTableStart = next;
            next = r.NumDescriptors == UINT_MAX ? UINT_MAX : r.OffsetInDescriptorsFromTableStart + r.NumDescriptors;
        }
    }
    rs->static_samplers_.assign(desc.pStaticSamplers, desc.pStaticSamplers + desc.NumStaticSamplers);
    rs->desc_ = desc;
    rs->desc_.pParameters = rs->root_params_.data();
    rs->desc_.pStaticSamplers = rs->static_samplers_.data();

    // Root data layout: root descriptors first, then the 32-bit words of constants and tables.
    uint32_t descriptors = 0, words = 0;
    for (const auto& p : rs->root_params_)
        if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV || p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV ||
            p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_UAV)
            descriptors++;
    const uint32_t words_base = 8 * descriptors;
    descriptors = 0;
    for (const auto& p : rs->root_params_) {
        Parameter out_p{p.ParameterType, 0, 0, 0, false};
        switch (p.ParameterType) {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            out_p.size = p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE ? 4 : 4 * p.Constants.Num32BitValues;
            out_p.sampler_table = p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE &&
                                  p.DescriptorTable.NumDescriptorRanges &&
                                  p.DescriptorTable.pDescriptorRanges[0].RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
            out_p.index = words;
            out_p.offset = words_base + 4 * words;
            words += out_p.size / 4;
            break;
        default:
            out_p.size = 8;
            out_p.index = descriptors;
            out_p.offset = 8 * descriptors++;
            break;
        }
        rs->params_.push_back(out_p);
    }
    rs->root_descriptors_ = descriptors;
    rs->root_words_ = words;
    rs->root_data_size_ = words_base + 4 * words;

    *out = std::move(rs);
    return S_OK;
}

} // namespace vgd
