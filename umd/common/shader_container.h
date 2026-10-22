// SPDX-License-Identifier: MIT
// Shader containers rebuilt from what the DDI gives: the engines take the API's DXBC containers, with named
// input/output signatures, but the runtime hands the driver the bare program (the DXBC token stream, or the DXIL
// program part) and register-based signatures without semantic names.
//
// The container holds ISG1, OSG1, PSG1 (hull and domain programs) and the program part (SHEX/SHDR or DXIL),
// unchanged. Names are a pure function of a signature entry, so the two sides of every stage boundary, and an input
// layout naming its registers, agree without seeing each other (the engines match all of them by name and index):
//   - system values get their SV_ name and index;
//   - pixel shader outputs without one are SV_Target<register>;
//   - every other entry is generic_semantic(stream) with index register * 4 + its first component.
#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

namespace vgd::umd {

// A signature entry, as both DDIs describe one (D3D12DDIARG_SIGNATURE_ENTRY_0012, D3D11_1DDIARG_SIGNATURE_ENTRY):
// the system value is a D3D10_SB_NAME, the component type a D3D10_SB_REGISTER_COMPONENT_TYPE and the minimum
// precision a D3D11_SB_OPERAND_MIN_PRECISION.
struct SignatureEntry {
    UINT system_value;
    UINT reg;
    BYTE mask;
    BYTE stream;
    UINT component_type;   // D3D10_SB_REGISTER_COMPONENT_*; 0 (unknown) gets one by system value (component_type_of)
    UINT min_precision;
};

struct ShaderSignatures {
    std::vector<SignatureEntry> input, output, patch_constant;
};

// Fills `checksum` with the container's DXBC checksum: the engines key their shader caches on it.
using DxbcChecksum = void (*)(const void* dxbc, size_t size, uint32_t checksum[4]);

// The container for `code` (pShaderCode). E_INVALIDARG if it is not a program we recognize.
HRESULT build_shader_container(const UINT* code, const ShaderSignatures& signatures, DxbcChecksum checksum,
                               std::vector<uint8_t>* out);
// A container holding only the input signature `input`, as API input layouts take one ("bytecode with input
// signature").
void build_input_signature_container(const std::vector<SignatureEntry>& input, DxbcChecksum checksum,
                                     std::vector<uint8_t>* out);

// The name of entries without a system value on output stream `stream` (0 except for geometry shader streams).
const char* generic_semantic(UINT stream);
// The semantic index of an entry without a system value at `reg` whose mask starts at component `first`.
inline UINT generic_semantic_index(UINT reg, UINT first) { return reg * 4 + first; }

// The semantic the container gives the output entry of `output` on `stream` at `reg` overlapping `mask`, and the
// first component of that entry. False if there is none.
bool output_semantic(const std::vector<SignatureEntry>& output, UINT stream, UINT reg, BYTE mask, const char** name,
                     UINT* index, UINT* first);

} // namespace vgd::umd
