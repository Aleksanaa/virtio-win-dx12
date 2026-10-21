// SPDX-License-Identifier: MIT
// Root signature deep copy and root data layout. No GPU needed.
#include <vgd/root_signature.h>

#include <cstdio>

#define CHECK(cond)                                                                                  \
    do {                                                                                             \
        if (!(cond)) {                                                                               \
            std::fprintf(stderr, "root_signature: FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            return 1;                                                                                \
        }                                                                                            \
    } while (0)

int main()
{
    D3D12_DESCRIPTOR_RANGE1 ranges[2] = {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 4},
    };
    D3D12_ROOT_PARAMETER1 params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 3};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor = {1, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE};
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable = {2, ranges};
    D3D12_ROOT_SIGNATURE_DESC1 desc{3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};

    std::unique_ptr<vgd::RootSignature> rs;
    CHECK(SUCCEEDED(vgd::RootSignature::create(desc, &rs)));
    auto p = rs->parameters();
    CHECK(p.size() == 3);
    // Root descriptors come first, then the words of constants and tables.
    CHECK(p[1].offset == 0 && p[1].size == 8 && p[1].index == 0);
    CHECK(p[0].offset == 8 && p[0].size == 12 && p[0].index == 0);
    CHECK(p[2].offset == 20 && p[2].size == 4 && p[2].index == 3);
    CHECK(rs->root_data_size() == 24 && rs->root_descriptor_count() == 1 && rs->root_word_count() == 4);

    // The copy must not alias the caller's memory.
    ranges[0].NumDescriptors = 99;
    params[0].Constants.Num32BitValues = 99;
    const auto& d = rs->desc();
    CHECK(d.pParameters != params && d.pParameters[0].Constants.Num32BitValues == 3);
    CHECK(d.pParameters[2].DescriptorTable.pDescriptorRanges != ranges);
    CHECK(d.pParameters[2].DescriptorTable.pDescriptorRanges[0].NumDescriptors == 4);
    CHECK(d.pParameters[2].DescriptorTable.pDescriptorRanges[1].OffsetInDescriptorsFromTableStart == 4);

    // Over the 64-DWORD limit.
    params[0].Constants.Num32BitValues = 62;   // 62 + 2 + 1 = 65
    CHECK(FAILED(vgd::RootSignature::create(desc, &rs)));

    std::fprintf(stderr, "root_signature: PASS\n");
    return 0;
}
