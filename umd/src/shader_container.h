// SPDX-License-Identifier: MIT
// Shader containers rebuilt from what the DDI gives: vkd3d-proton takes the API's DXBC containers, with named
// input/output signatures, but the runtime hands the driver the bare program (the DXBC token stream, or the DXIL
// program part) and register-based signatures without semantic names.
//
// The container holds ISG1, OSG1, PSG1 (hull and domain programs) and the program part (SHEX/SHDR or DXIL),
// unchanged. Names are a pure function of a signature entry, so the two sides of every stage boundary, and an input
// layout naming its registers, agree without seeing each other (vkd3d-proton matches all of them by name and index):
//   - system values get their SV_ name and index;
//   - pixel shader outputs without one are SV_Target<register>;
//   - every other entry is generic_semantic(stream) with index register * 4 + its first component.
#pragma once

#include "wdk.h"

#include <cstdint>
#include <vector>

namespace vgd::umd {

struct ShaderSignatures {
    const D3D12DDIARG_SIGNATURE_ENTRY_0012* input = nullptr;
    UINT input_count = 0;
    const D3D12DDIARG_SIGNATURE_ENTRY_0012* output = nullptr;
    UINT output_count = 0;
    const D3D12DDIARG_SIGNATURE_ENTRY_0012* patch_constant = nullptr;
    UINT patch_constant_count = 0;
};

// The container for `code` (pShaderCode). E_INVALIDARG if it is not a program we recognize.
HRESULT build_shader_container(const UINT* code, const ShaderSignatures& signatures, std::vector<uint8_t>* out);

// The name of entries without a system value on output stream `stream` (0 except for geometry shader streams).
const char* generic_semantic(UINT stream);
// The semantic index of an entry without a system value at `reg` whose mask starts at component `first`.
inline UINT generic_semantic_index(UINT reg, UINT first) { return reg * 4 + first; }

// The semantic the container gives the output entry of `output` on `stream` at `reg` overlapping `mask`, and the
// first component of that entry. False if there is none.
bool output_semantic(const D3D12DDIARG_SIGNATURE_ENTRY_0012* output, UINT count, UINT stream, UINT reg, BYTE mask,
                     const char** name, UINT* index, UINT* first);

} // namespace vgd::umd
