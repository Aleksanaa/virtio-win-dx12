# SPDX-License-Identifier: MIT
# venera-panel.exe, the control panel (panel.h): the window, and with a command the command line (cli.cpp), including
# the graphics probe and the tests (tests/), which the window and the command line run as child processes of it.
# venera-panel.com (shim.c) is its console face for shells.
enable_language(RC)
add_executable(venera-panel WIN32 main.cpp ui.cpp overview.cpp testspage.cpp logspage.cpp logcapture.cpp
    settingspage.cpp toolspage.cpp fpsoverlay.cpp report.cpp sysinfo.cpp probe.cpp cli.cpp
    tests/runner.cpp tests/d3d12.cpp tests/d3d11.cpp tests/d3d9.cpp tests/vulkan.cpp panel.rc)
target_compile_definitions(venera-panel PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_include_directories(venera-panel PRIVATE tests/shaders)
target_link_libraries(venera-panel PRIVATE venera-vulkan-headers venera-directx-headers
    d2d1 dwrite dwmapi d3d12 d3d11 d3d9 dxgi dxguid opengl32 setupapi cfgmgr32 shcore ntdll psapi gdi32 user32 shell32
    advapi32 comdlg32)
target_link_options(venera-panel PRIVATE -municode -static -static-libgcc -static-libstdc++)
set_source_files_properties(panel.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/docs/venera.ico")

add_executable(venera-panel-com shim.c)
set_target_properties(venera-panel-com PROPERTIES OUTPUT_NAME venera-panel SUFFIX .com)
target_compile_definitions(venera-panel-com PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_link_options(venera-panel-com PRIVATE -municode -static)
