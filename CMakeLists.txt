# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.24)
project(vgd3d12 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(VGD_WITH_DXIL_SPIRV "Build dxil-spirv for the shader path" ON)
set(VGD_WINKITS_DIR "${CMAKE_SOURCE_DIR}/refs/winkits" CACHE PATH
    "WDK/SDK headers extracted from the Microsoft.Windows.WDK.x64 / SDK.CPP NuGet packages (umd only)")

# Third-party (git submodules, pinned).
add_library(vgd-vulkan-headers INTERFACE)
target_include_directories(vgd-vulkan-headers SYSTEM INTERFACE third_party/Vulkan-Headers/include)
add_library(vgd-directx-headers INTERFACE)
target_include_directories(vgd-directx-headers SYSTEM INTERFACE
    third_party/DirectX-Headers/include third_party/DirectX-Headers/include/directx)
if(NOT WIN32)
  target_include_directories(vgd-directx-headers SYSTEM INTERFACE third_party/DirectX-Headers/include/wsl/stubs)
endif()

if(VGD_WITH_DXIL_SPIRV)
  set(DXIL_SPIRV_CLI OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/dxil-spirv EXCLUDE_FROM_ALL)
endif()

add_subdirectory(core)
if(WIN32)
  add_subdirectory(umd)
  add_subdirectory(tests/win)
else()
  enable_testing()
  add_subdirectory(core/tests)
endif()
