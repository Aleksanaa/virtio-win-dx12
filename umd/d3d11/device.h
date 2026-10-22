// SPDX-License-Identifier: MIT
#pragma once

#include "dxvk_bridge.h"
#include "objects.h"
#include "viogpu_abi.h"

namespace vgd::umd {

// Creates the device in args.hDrvDevice over `d3d` (a DXVK device of the requested level) and fills the runtime's
// device and DXGI tables.
HRESULT create_device(D3D10DDIARG_CREATEDEVICE& args, const viogpu::AdapterInfo& info, PFN_vkGetInstanceProcAddr gipa,
                      Com<ID3D11Device5> d3d, const dxvk::DeviceHandles& handles);

} // namespace vgd::umd
