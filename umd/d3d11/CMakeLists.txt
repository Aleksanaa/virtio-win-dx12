# SPDX-License-Identifier: MIT
# vgd3d11.dll: the D3D10/11 UMD, a DDI shell over DXVK.
vgd_generate_ddi_slots(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/ddi_slots.h" TABLES
    VGD_SLOTS_D3D11_1_DEVICE  "${WDK_INC}/um/d3d10umddi.h"  D3D11_1DDI_DEVICEFUNCS
    VGD_SLOTS_DXGI_1_2        "${SDK_INC}/um/dxgiddi.h"     DXGI1_2_DDI_BASE_FUNCTIONS)

# The one source that sees DXVK's headers (see dxvk_bridge.cpp), compiled as DXVK compiles its own.
add_library(vgd-dxvk-bridge OBJECT dxvk_bridge.cpp)
# DXVK's headers include each other relative to the directory of the source including them (meson adds it).
target_include_directories(vgd-dxvk-bridge PRIVATE "${DXVK_SRC}/src" "${DXVK_SRC}/src/d3d11"
    "${DXVK_SRC}/subprojects/dxbc-spirv" ../common)
target_link_libraries(vgd-dxvk-bridge PRIVATE vgd-dxvk)
set_target_properties(vgd-dxvk-bridge PROPERTIES CXX_STANDARD 17)

add_library(vgd3d11 SHARED
    adapter.cpp
    device.cpp
    draw.cpp
    format.cpp
    pipeline.cpp
    present.cpp
    query.cpp
    resource.cpp
    view.cpp
    $<TARGET_OBJECTS:vgd-dxvk-bridge>)
# Next to vgd3d12.dll in the build tree's umd/, where vgd-test.ps1 finds them.
set_target_properties(vgd3d11 PROPERTIES PREFIX "" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/umd")
target_include_directories(vgd3d11 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(vgd3d11 PRIVATE vgd-umd-common vgd-vulkan-headers vgd-dxvk)
target_compile_options(vgd3d11 PRIVATE ${VGD_UMD_WARNINGS} ${VGD_UMD_WDK_INCLUDES})
target_link_options(vgd3d11 PRIVATE ${VGD_UMD_LINK_OPTIONS})
