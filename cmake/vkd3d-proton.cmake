# SPDX-License-Identifier: MIT
# vkd3d-proton (third_party/vkd3d-proton, vendored) built with its own meson build for the MinGW target, as static
# libraries the UMD links in (targets at the end).
#
# meson and ninja come from the dev shell, as do widl (IDL headers) and glslang (its internal shaders). The meson
# build is configured once in the build tree; every build runs its ninja, which does nothing when nothing changed.

set(VKD3D_SRC "${CMAKE_SOURCE_DIR}/third_party/vkd3d-proton")
set(VKD3D_BUILD "${CMAKE_BINARY_DIR}/vkd3d-proton")

find_program(VGD_MESON meson REQUIRED)
find_program(VGD_NINJA ninja REQUIRED)

# GCC from nixpkgs uses the mcfgthread model, whose headers and library live outside the wrapper's paths.
set(VKD3D_C_ARGS "")
set(VKD3D_LINK_ARGS "")
if(DEFINED ENV{VGD_MINGW_MCFG_INCLUDE})
  set(VKD3D_C_ARGS "'-isystem', '$ENV{VGD_MINGW_MCFG_INCLUDE}'")
endif()
if(DEFINED ENV{VGD_MINGW_MCFG_LIB})
  set(VKD3D_LINK_ARGS "'-L$ENV{VGD_MINGW_MCFG_LIB}'")
endif()
file(WRITE "${VKD3D_BUILD}/cross.ini" "\
[binaries]
c = '${CMAKE_C_COMPILER}'
cpp = '${CMAKE_CXX_COMPILER}'
ar = 'x86_64-w64-mingw32-ar'
strip = 'x86_64-w64-mingw32-strip'
windres = 'x86_64-w64-mingw32-windres'

[built-in options]
c_args = [${VKD3D_C_ARGS}]
cpp_args = [${VKD3D_C_ARGS}]
c_link_args = [${VKD3D_LINK_ARGS}]
cpp_link_args = [${VKD3D_LINK_ARGS}]

[properties]
needs_exe_wrapper = true

[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
")

if(NOT EXISTS "${VKD3D_BUILD}/meson/build.ninja")
  execute_process(
    COMMAND "${VGD_MESON}" setup "${VKD3D_BUILD}/meson" "${VKD3D_SRC}" --cross-file "${VKD3D_BUILD}/cross.ini"
            --buildtype=release -Db_ndebug=true
    RESULT_VARIABLE vkd3d_setup)
  if(NOT vkd3d_setup EQUAL 0)
    message(FATAL_ERROR "meson setup of vkd3d-proton failed")
  endif()
endif()

# Link order: users before what they use.
set(VKD3D_LIBS
    "${VKD3D_BUILD}/meson/libs/vkd3d/libvkd3d-proton.a"
    "${VKD3D_BUILD}/meson/libs/vkd3d-shader/libvkd3d-shader.a"
    "${VKD3D_BUILD}/meson/libs/vkd3d-common/libvkd3d_common.a"
    "${VKD3D_BUILD}/meson/subprojects/dxil-spirv/libdxil-spirv.a"
    "${VKD3D_BUILD}/meson/subprojects/dxil-spirv/libdxbc_spv_module.a"
    "${VKD3D_BUILD}/meson/subprojects/dxil-spirv/subprojects/dxbc-spirv/libdxbc_spv.a")
set(VKD3D_TARGETS)
foreach(lib ${VKD3D_LIBS})
  file(RELATIVE_PATH rel "${VKD3D_BUILD}/meson" "${lib}")
  list(APPEND VKD3D_TARGETS "${rel}")
endforeach()

add_custom_target(vkd3d-proton-build
  COMMAND "${VGD_NINJA}" -C "${VKD3D_BUILD}/meson" ${VKD3D_TARGETS}
  BYPRODUCTS ${VKD3D_LIBS}
  COMMENT "Building vkd3d-proton (meson)"
  USES_TERMINAL)

# Two targets: the libraries (with the Windows libraries they need), and the headers, which only the C bridge into
# vkd3d-proton includes (umd/src/vkd3d_bridge.c): its IDL-generated D3D12 headers clash with Microsoft's.
add_library(vgd-vkd3d-proton INTERFACE)
add_dependencies(vgd-vkd3d-proton vkd3d-proton-build)
# The archives reference each other (dxil-spirv's C API and dxbc-spirv), so the group resolves them in any order.
target_link_libraries(vgd-vkd3d-proton INTERFACE
    -Wl,--start-group ${VKD3D_LIBS} -Wl,--end-group
    version pathcch psapi ntdll dxgi)

add_library(vgd-vkd3d-proton-headers INTERFACE)
add_dependencies(vgd-vkd3d-proton-headers vkd3d-proton-build)
target_include_directories(vgd-vkd3d-proton-headers SYSTEM INTERFACE
    "${VKD3D_SRC}/include"
    # The IDL headers (vkd3d_d3d12.h, ...): meson generates them for each target, vkd3d-common's are complete.
    "${VKD3D_BUILD}/meson/libs/vkd3d-common/libvkd3d_common.a.p"
    "${VKD3D_SRC}/khronos/Vulkan-Headers/include")
