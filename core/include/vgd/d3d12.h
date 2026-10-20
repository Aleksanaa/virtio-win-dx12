// SPDX-License-Identifier: MIT
// D3D12 API types on every platform: the Windows SDK view on Windows, DirectX-Headers' WSL adapter elsewhere.
#pragma once

#ifndef _WIN32
#include <wsl/winadapter.h>
#endif
#include <directx/d3d12.h>
