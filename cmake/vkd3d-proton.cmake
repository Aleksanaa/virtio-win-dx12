# SPDX-License-Identifier: MIT
# vkd3d-proton (third_party/vkd3d-proton, vendored) built with its own meson build for the MinGW target, as static
# libraries the D3D12 UMD links in (targets at the end). widl (IDL headers) and glslang (its internal shaders) come
# from the dev shell.

set(VKD3D_SRC "${CMAKE_SOURCE_DIR}/third_party/vkd3d-proton")
set(VKD3D_BUILD "${CMAKE_BINARY_DIR}/vkd3d-proton")

vgd_meson_cross_file("${VKD3D_BUILD}/cross.ini")
vgd_meson_setup(vkd3d-proton "${VKD3D_SRC}" "${VKD3D_BUILD}/meson" "${VKD3D_BUILD}/cross.ini")

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
    version pathcch psapi ntdll gdi32)

add_library(vgd-vkd3d-proton-headers INTERFACE)
add_dependencies(vgd-vkd3d-proton-headers vkd3d-proton-build)
target_include_directories(vgd-vkd3d-proton-headers SYSTEM INTERFACE
    "${VKD3D_SRC}/include"
    # The IDL headers (vkd3d_d3d12.h, ...): meson generates them for each target, vkd3d-common's are complete.
    "${VKD3D_BUILD}/meson/libs/vkd3d-common/libvkd3d_common.a.p"
    "${VKD3D_SRC}/khronos/Vulkan-Headers/include")
