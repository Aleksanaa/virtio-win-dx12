# SPDX-License-Identifier: MIT
# vgd3d12.dll: the D3D12 UMD. WDK headers are searched after MinGW's own (-idirafter), so MinGW's windows.h
# wins and only the DDI headers MinGW lacks come from the WDK.
set(WDK_INC "${VGD_WINKITS_DIR}/wdk/c/Include/10.0.26100.0")
set(SDK_INC "${VGD_WINKITS_DIR}/sdk/c/Include/10.0.26100.0")
if(NOT EXISTS "${WDK_INC}/um/d3d12umddi.h")
  message(FATAL_ERROR "d3d12umddi.h not found under ${WDK_INC}; see plan.md (Toolchain) for fetching the WDK headers")
endif()

add_library(vgd3d12 SHARED
    src/adapter.cpp
    src/log.cpp)
set_target_properties(vgd3d12 PROPERTIES PREFIX "")
target_link_libraries(vgd3d12 PRIVATE vgd-core)
target_compile_options(vgd3d12 PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
    "SHELL:-idirafter ${WDK_INC}/um" "SHELL:-idirafter ${WDK_INC}/shared"
    "SHELL:-idirafter ${SDK_INC}/shared" "SHELL:-idirafter ${SDK_INC}/um")
# Self-contained DLL: the runtime loads it into arbitrary processes, so no MinGW runtime DLLs alongside.
target_link_options(vgd3d12 PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--exclude-all-symbols)
