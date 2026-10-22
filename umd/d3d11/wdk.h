// SPDX-License-Identifier: MIT
// WDK display DDI headers on MinGW: MinGW's windows.h and D3D11 API first, then the WDK's d3d10umddi.h (found via
// -idirafter).
#pragma once

#include <windows.h>

#include <d3d11_4.h>
#include <dxgi1_6.h>

// Scoped to the WDK include: libstdc++ uses __out as an identifier.
#pragma push_macro("__out")
#undef __out
#define __out
#include <d3d10umddi.h>
#pragma pop_macro("__out")
