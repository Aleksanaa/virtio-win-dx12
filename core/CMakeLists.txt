# SPDX-License-Identifier: MIT
add_library(vgd-core STATIC
    src/instance.cpp
    src/device.cpp
    src/commands.cpp
    src/formats.cpp
    src/root_signature.cpp
    src/pipeline.cpp)
target_include_directories(vgd-core PUBLIC include PRIVATE src)
target_link_libraries(vgd-core PUBLIC vgd-vulkan-headers vgd-directx-headers)
target_compile_options(vgd-core PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
if(VGD_WITH_DXIL_SPIRV)
  target_link_libraries(vgd-core PRIVATE dxil-spirv-c-static)
endif()
