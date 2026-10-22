// SPDX-License-Identifier: MIT
// WDK display DDI headers on MinGW: MinGW's windows.h first, then the D3D12 API, then the WDK's d3d12umddi.h
// (found via -idirafter), with the few SAL annotations MinGW's sal.h lacks.
#pragma once

#include <windows.h>

// The D3D12 API (DirectX-Headers): vkd3d-proton's objects are driven through it.
#include <directx/d3d12.h>
#include <dxguids/dxguids.h>

// Scoped to the WDK include: libstdc++ uses __out as an identifier.
#pragma push_macro("__out")
#undef __out
#define __out
#include <d3d12umddi.h>
#pragma pop_macro("__out")
