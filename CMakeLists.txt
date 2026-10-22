# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.24)
project(vgd3d12 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT WIN32)
  message(FATAL_ERROR "vgd3d12 builds for the Windows guest only: configure with -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake")
endif()

set(VGD_WINKITS_DIR "${CMAKE_SOURCE_DIR}/refs/winkits" CACHE PATH
    "WDK/SDK headers extracted from the Microsoft.Windows.WDK.x64 / SDK.CPP NuGet packages (umd only)")

# Third-party headers: the Vulkan headers vkd3d-proton pins, and DirectX-Headers (git submodule, pinned).
add_library(vgd-vulkan-headers INTERFACE)
target_include_directories(vgd-vulkan-headers SYSTEM INTERFACE third_party/vkd3d-proton/khronos/Vulkan-Headers/include)
add_library(vgd-directx-headers INTERFACE)
target_include_directories(vgd-directx-headers SYSTEM INTERFACE
    third_party/DirectX-Headers/include third_party/DirectX-Headers/include/directx)

# The D3D12 implementation: vkd3d-proton (vendored), built with meson.
include(cmake/vkd3d-proton.cmake)

add_subdirectory(umd)
add_subdirectory(tests/win)
