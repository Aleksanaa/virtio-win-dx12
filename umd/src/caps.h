// SPDX-License-Identifier: MIT
#pragma once

#include "wdk.h"

namespace vgd::umd {

// Answers from `device`, the engine's (engine.h).
HRESULT get_caps(ID3D12Device* device, const D3D12DDIARG_GETCAPS& args);

} // namespace vgd::umd
