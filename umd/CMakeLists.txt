# SPDX-License-Identifier: MIT
# The user-mode drivers: venera_d3d12.dll (D3D12, on vkd3d-proton) and venera_d3d11.dll (D3D10/11, on DXVK), and what
# they share (common/). WDK headers are searched after MinGW's own (-idirafter), so MinGW's windows.h wins and only the
# DDI headers MinGW lacks come from the WDK.
set(WDK_INC "${VENERA_WINKITS_DIR}/wdk/c/Include/10.0.26100.0")
set(SDK_INC "${VENERA_WINKITS_DIR}/sdk/c/Include/10.0.26100.0")
if(NOT EXISTS "${WDK_INC}/um/d3d12umddi.h")
  message(FATAL_ERROR "d3d12umddi.h not found under ${WDK_INC}; see plan.md (Build) for fetching the WDK headers")
endif()

set(VENERA_UMD_WARNINGS -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
set(VENERA_UMD_WDK_INCLUDES
    "SHELL:-idirafter ${WDK_INC}/um" "SHELL:-idirafter ${WDK_INC}/shared"
    "SHELL:-idirafter ${SDK_INC}/shared" "SHELL:-idirafter ${SDK_INC}/um")
# Self-contained DLLs: the runtime loads them into arbitrary processes, so no MinGW runtime DLLs alongside.
set(VENERA_UMD_LINK_OPTIONS -static -static-libgcc -static-libstdc++ -Wl,--exclude-all-symbols)

# Slot names of the DDI tables we fill, for logging stubs (see cmake/ddi_slots.cmake).
include(${PROJECT_SOURCE_DIR}/cmake/ddi_slots.cmake)

# --- Shared --------------------------------------------------------------------------------------------------------
add_library(venera-umd-common STATIC
    common/kmt.cpp
    common/linear_image.cpp
    common/log.cpp
    common/shared_surface.cpp
    common/shader_container.cpp
    common/stacks.cpp
    common/vulkan_device.cpp
    common/vulkan_icd.cpp)
target_include_directories(venera-umd-common PUBLIC common)
target_link_libraries(venera-umd-common PUBLIC venera-vulkan-headers)
target_compile_options(venera-umd-common PRIVATE ${VENERA_UMD_WARNINGS} ${VENERA_UMD_WDK_INCLUDES})

add_subdirectory(d3d12)
add_subdirectory(d3d11)
