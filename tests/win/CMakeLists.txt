# SPDX-License-Identifier: MIT
add_executable(d3dprobe d3dprobe.cpp)
target_link_libraries(d3dprobe PRIVATE vgd-directx-headers d3d12 dxgi dxguid)
target_include_directories(d3dprobe PRIVATE ${CMAKE_SOURCE_DIR}/tests/shaders)
target_link_options(d3dprobe PRIVATE -static -static-libgcc -static-libstdc++)
