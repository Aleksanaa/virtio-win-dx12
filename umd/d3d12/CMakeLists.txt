# SPDX-License-Identifier: MIT
# vgd3d12.dll: the D3D12 UMD, a DDI shell over vkd3d-proton.
set(D3D12UMDDI "${WDK_INC}/um/d3d12umddi.h")
vgd_generate_ddi_slots(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/ddi_slots.h" TABLES
    VGD_SLOTS_DEVICE_CORE_0088       "${D3D12UMDDI}"            D3D12DDI_DEVICE_FUNCS_CORE_0088
    VGD_SLOTS_COMMAND_LIST_3D_0092   "${D3D12UMDDI}"            D3D12DDI_COMMAND_LIST_FUNCS_3D_0092
    VGD_SLOTS_COMMAND_QUEUE_CORE_0001 "${D3D12UMDDI}"           D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001
    VGD_SLOTS_EXTENDED_FEATURES_0021 "${D3D12UMDDI}"            D3D12DDI_EXTENDED_FEATURES_FUNCS_0021
    VGD_SLOTS_DXGI_1_4               "${SDK_INC}/um/dxgiddi.h"  DXGI1_4_DDI_BASE_FUNCTIONS)

# The one source that sees vkd3d-proton's headers (see vkd3d_bridge.c).
add_library(vgd-vkd3d-bridge OBJECT vkd3d_bridge.c)
target_link_libraries(vgd-vkd3d-bridge PRIVATE vgd-vkd3d-proton-headers)
add_dependencies(vgd-vkd3d-bridge vkd3d-proton-build)

add_library(vgd3d12 SHARED
    adapter.cpp
    caps.cpp
    device.cpp
    device_core.cpp
    pipeline_ddi.cpp
    command_ddi.cpp
    queue_bridge.cpp
    resource_ddi.cpp
    descriptor_ddi.cpp
    query_ddi.cpp
    indirect_ddi.cpp
    present.cpp
    engine.cpp
    shared_image.cpp
    $<TARGET_OBJECTS:vgd-vkd3d-bridge>)
# Next to each other in the build tree's umd/, where vgd-test.ps1 finds them.
set_target_properties(vgd3d12 PROPERTIES PREFIX "" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/umd")
target_include_directories(vgd3d12 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(vgd3d12 PRIVATE vgd-umd-common vgd-vulkan-headers vgd-directx-headers vgd-vkd3d-proton)
target_compile_options(vgd3d12 PRIVATE ${VGD_UMD_WARNINGS} ${VGD_UMD_WDK_INCLUDES})
target_link_options(vgd3d12 PRIVATE ${VGD_UMD_LINK_OPTIONS})
