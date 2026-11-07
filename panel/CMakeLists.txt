# SPDX-License-Identifier: MIT
# vgd-panel.exe, the control panel (panel.h). One executable: the window, and with --probe the graphics probe it runs
# as a child process.
enable_language(RC)
add_executable(vgd-panel WIN32 main.cpp ui.cpp overview.cpp mockpages.cpp sysinfo.cpp probe.cpp panel.rc)
target_compile_definitions(vgd-panel PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_link_libraries(vgd-panel PRIVATE vgd-vulkan-headers vgd-directx-headers
    d2d1 dwrite dwmapi d3d12 d3d11 dxgi dxguid opengl32 setupapi cfgmgr32 shcore ntdll gdi32 user32 shell32 advapi32)
target_link_options(vgd-panel PRIVATE -municode -static -static-libgcc -static-libstdc++)
