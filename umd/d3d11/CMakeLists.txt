# SPDX-License-Identifier: MIT
# venera_d3d11.dll: the D3D10/11 UMD, a DDI shell over DXVK.
venera_generate_ddi_slots(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/ddi_slots.h" TABLES
    VENERA_SLOTS_D3D11_1_DEVICE  "${WDK_INC}/um/d3d10umddi.h"  D3D11_1DDI_DEVICEFUNCS
    VENERA_SLOTS_DXGI_1_2        "${SDK_INC}/um/dxgiddi.h"     DXGI1_2_DDI_BASE_FUNCTIONS)

# The one source that sees DXVK's headers (see dxvk_bridge.cpp), compiled as DXVK compiles its own.
add_library(venera-dxvk-bridge OBJECT dxvk_bridge.cpp)
# DXVK's headers include each other relative to the directory of the source including them (meson adds it).
target_include_directories(venera-dxvk-bridge PRIVATE "${DXVK_SRC}/src" "${DXVK_SRC}/src/d3d11"
    "${DXVK_SRC}/subprojects/dxbc-spirv" ../common)
target_link_libraries(venera-dxvk-bridge PRIVATE venera-dxvk)
set_target_properties(venera-dxvk-bridge PROPERTIES CXX_STANDARD 17)

add_library(venera_d3d11 SHARED
    adapter.cpp
    device.cpp
    draw.cpp
    format.cpp
    pipeline.cpp
    present.cpp
    query.cpp
    resource.cpp
    view.cpp
    $<TARGET_OBJECTS:venera-dxvk-bridge>)
# Next to venera_d3d12.dll in the build tree's umd/, where venera-test.ps1 finds them.
set_target_properties(venera_d3d11 PROPERTIES PREFIX "" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/umd")
target_include_directories(venera_d3d11 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(venera_d3d11 PRIVATE venera-umd-common venera-vulkan-headers venera-dxvk)
target_compile_options(venera_d3d11 PRIVATE ${VENERA_UMD_WARNINGS} ${VENERA_UMD_WDK_INCLUDES})
target_link_options(venera_d3d11 PRIVATE ${VENERA_UMD_LINK_OPTIONS})
