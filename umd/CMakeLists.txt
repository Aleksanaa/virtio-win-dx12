# SPDX-License-Identifier: MIT
# vgd3d12.dll: the D3D12 UMD. WDK headers are searched after MinGW's own (-idirafter), so MinGW's windows.h
# wins and only the DDI headers MinGW lacks come from the WDK.
set(WDK_INC "${VGD_WINKITS_DIR}/wdk/c/Include/10.0.26100.0")
set(SDK_INC "${VGD_WINKITS_DIR}/sdk/c/Include/10.0.26100.0")
if(NOT EXISTS "${WDK_INC}/um/d3d12umddi.h")
  message(FATAL_ERROR "d3d12umddi.h not found under ${WDK_INC}; see plan.md (Toolchain) for fetching the WDK headers")
endif()

# Slot names of the DDI tables we fill, for logging stubs (see cmake/ddi_slots.cmake).
include(${PROJECT_SOURCE_DIR}/cmake/ddi_slots.cmake)
set(D3D12UMDDI "${WDK_INC}/um/d3d12umddi.h")
vgd_generate_ddi_slots(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/ddi_slots.h" TABLES
    VGD_SLOTS_DEVICE_CORE_0088       "${D3D12UMDDI}"            D3D12DDI_DEVICE_FUNCS_CORE_0088
    VGD_SLOTS_COMMAND_LIST_3D_0092   "${D3D12UMDDI}"            D3D12DDI_COMMAND_LIST_FUNCS_3D_0092
    VGD_SLOTS_COMMAND_QUEUE_CORE_0001 "${D3D12UMDDI}"           D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001
    VGD_SLOTS_EXTENDED_FEATURES_0021 "${D3D12UMDDI}"            D3D12DDI_EXTENDED_FEATURES_FUNCS_0021
    VGD_SLOTS_DXGI_1_4               "${SDK_INC}/um/dxgiddi.h"  DXGI1_4_DDI_BASE_FUNCTIONS)

add_library(vgd3d12 SHARED
    src/adapter.cpp
    src/caps.cpp
    src/device.cpp
    src/device_core.cpp
    src/pipeline_ddi.cpp
    src/command_ddi.cpp
    src/queue_bridge.cpp
    src/resource_ddi.cpp
    src/descriptor_ddi.cpp
    src/query_ddi.cpp
    src/indirect_ddi.cpp
    src/log.cpp
    src/vulkan_icd.cpp)
set_target_properties(vgd3d12 PROPERTIES PREFIX "")
target_include_directories(vgd3d12 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(vgd3d12 PRIVATE vgd-core)
target_compile_options(vgd3d12 PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
    "SHELL:-idirafter ${WDK_INC}/um" "SHELL:-idirafter ${WDK_INC}/shared"
    "SHELL:-idirafter ${SDK_INC}/shared" "SHELL:-idirafter ${SDK_INC}/um")
# Self-contained DLL: the runtime loads it into arbitrary processes, so no MinGW runtime DLLs alongside.
target_link_options(vgd3d12 PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--exclude-all-symbols)
