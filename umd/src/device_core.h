// SPDX-License-Identifier: MIT
#pragma once

#include "wdk.h"

namespace vgd::umd {

// Overrides the stubbed device table with the slots implemented so far.
void fill_device_core(D3D12DDI_DEVICE_FUNCS_CORE_0088& table);

} // namespace vgd::umd
