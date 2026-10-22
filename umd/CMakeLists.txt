# SPDX-License-Identifier: MIT
# The user-mode drivers: vgd3d12.dll (D3D12, on vkd3d-proton), and what UMDs share (common/). WDK headers are
# searched after MinGW's own (-idirafter), so MinGW's windows.h wins and only the DDI headers MinGW lacks come from
# the WDK.
set(WDK_INC "${VGD_WINKITS_DIR}/wdk/c/Include/10.0.26100.0")
set(SDK_INC "${VGD_WINKITS_DIR}/sdk/c/Include/10.0.26100.0")
if(NOT EXISTS "${WDK_INC}/um/d3d12umddi.h")
  message(FATAL_ERROR "d3d12umddi.h not found under ${WDK_INC}; see plan.md (Build) for fetching the WDK headers")
endif()

set(VGD_UMD_WARNINGS -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
set(VGD_UMD_WDK_INCLUDES
    "SHELL:-idirafter ${WDK_INC}/um" "SHELL:-idirafter ${WDK_INC}/shared"
    "SHELL:-idirafter ${SDK_INC}/shared" "SHELL:-idirafter ${SDK_INC}/um")
# Self-contained DLLs: the runtime loads them into arbitrary processes, so no MinGW runtime DLLs alongside.
set(VGD_UMD_LINK_OPTIONS -static -static-libgcc -static-libstdc++ -Wl,--exclude-all-symbols)

# Slot names of the DDI tables we fill, for logging stubs (see cmake/ddi_slots.cmake).
include(${PROJECT_SOURCE_DIR}/cmake/ddi_slots.cmake)

# --- Shared --------------------------------------------------------------------------------------------------------
add_library(vgd-umd-common STATIC
    common/kmt.cpp
    common/linear_image.cpp
    common/log.cpp
    common/shader_container.cpp
    common/vulkan_device.cpp
    common/vulkan_icd.cpp)
target_include_directories(vgd-umd-common PUBLIC common)
target_link_libraries(vgd-umd-common PUBLIC vgd-vulkan-headers)
target_compile_options(vgd-umd-common PRIVATE ${VGD_UMD_WARNINGS} ${VGD_UMD_WDK_INCLUDES})

add_subdirectory(d3d12)
