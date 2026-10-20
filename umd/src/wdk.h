// SPDX-License-Identifier: MIT
// WDK display DDI headers on MinGW: MinGW's windows.h first, then the WDK's d3d12umddi.h (found via -idirafter),
// with the few SAL annotations MinGW's sal.h lacks.
#pragma once

#include <windows.h>

// Scoped to the WDK include: libstdc++ uses __out as an identifier.
#pragma push_macro("__out")
#undef __out
#define __out
#include <d3d12umddi.h>
#pragma pop_macro("__out")
