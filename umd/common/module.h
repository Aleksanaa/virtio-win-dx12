// SPDX-License-Identifier: MIT
#pragma once

#include <windows.h>

namespace vgd::umd {

// For DllMain at DLL_PROCESS_ATTACH: keeps the DLL loaded until the process exits, whatever FreeLibrary calls the
// runtime makes. Code of ours outlives the devices: libgcc, linked in statically, implements thread_local with
// emulated TLS, whose destructor runs at every thread's exit, and DXVK's and vkd3d-proton's threads and our exception
// logger may still be live. Unloaded, the next thread to exit called into unmapped memory: consent.exe (the UAC
// prompt), which closes its D3D11 device and runs on, crashed (BEX64, c0000005 in vgd3d11.dll_unloaded, libgcc's
// emutls_destroy) and no program could be started elevated.
inline void pin_module(HINSTANCE self)
{
    HMODULE pinned;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(self), &pinned);
}

} // namespace vgd::umd
