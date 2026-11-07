# SPDX-License-Identifier: MIT
add_executable(vkprobe vkprobe.cpp)
target_link_libraries(vkprobe PRIVATE vgd-vulkan-headers)
target_link_options(vkprobe PRIVATE -static -static-libgcc -static-libstdc++)

add_executable(vkdeviceprobe vkdeviceprobe.cpp)
target_link_libraries(vkdeviceprobe PRIVATE vgd-vulkan-headers)
target_link_options(vkdeviceprobe PRIVATE -static -static-libgcc -static-libstdc++)

# The guest scripts next to the probes, so a copy of build/ is all the guest needs (they find the rest from there).
foreach(script vgd-test.ps1 kmt-trace.wds)
  configure_file(${script} ${script} COPYONLY)
endforeach()

add_executable(stackdump stackdump.cpp)
target_link_libraries(stackdump PRIVATE dbghelp)
target_link_options(stackdump PRIVATE -static -static-libgcc -static-libstdc++)

add_executable(d3d11caps d3d11caps.cpp)
target_link_libraries(d3d11caps PRIVATE d3d11 dxgi dxguid)
target_link_options(d3d11caps PRIVATE -static -static-libgcc -static-libstdc++)

add_executable(uibench uibench.cpp)
target_link_libraries(uibench PRIVATE dwmapi winmm user32 gdi32)
target_link_options(uibench PRIVATE -static -static-libgcc -static-libstdc++)
