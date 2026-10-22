# SPDX-License-Identifier: MIT
# DXVK (third_party/dxvk, git submodule) built with its own meson build for the MinGW target: its D3D11
# implementation, which vgd3d11.dll links in (targets at the end). glslang (its internal shaders) comes from the dev
# shell. Only the D3D11 parts are built: no D3D9/D3D8, and no DLLs (dxvk-objects.sh).

set(DXVK_SRC "${CMAKE_SOURCE_DIR}/third_party/dxvk")
set(DXVK_BUILD "${CMAKE_BINARY_DIR}/dxvk")

vgd_meson_cross_file("${DXVK_BUILD}/cross.ini")
vgd_meson_setup(dxvk "${DXVK_SRC}" "${DXVK_BUILD}/meson" "${DXVK_BUILD}/cross.ini"
    -Denable_d3d9=false -Denable_d3d8=false -Denable_d3d10=false)

# Link order: users before what they use. The first is d3d11.dll's objects (dxvk-objects.sh).
set(DXVK_LIBS
    "${DXVK_BUILD}/libvgd-dxvk-d3d11.a"
    "${DXVK_BUILD}/meson/src/dxvk/libdxvk.a"
    "${DXVK_BUILD}/meson/src/wsi/libwsi.a"
    "${DXVK_BUILD}/meson/src/vulkan/libvkcommon.a"
    "${DXVK_BUILD}/meson/src/spirv/libspirv.a"
    "${DXVK_BUILD}/meson/src/util/libutil.a"
    "${DXVK_BUILD}/meson/subprojects/dxbc-spirv/libdxbc_spv.a"
    "${DXVK_BUILD}/meson/subprojects/libdisplay-info/libdisplay-info.a")
set(DXVK_MESON_LIBS ${DXVK_LIBS})
list(REMOVE_AT DXVK_MESON_LIBS 0)

add_custom_target(dxvk-build
  COMMAND sh "${CMAKE_SOURCE_DIR}/cmake/dxvk-objects.sh" "${VGD_NINJA}" "${CMAKE_AR}" "${DXVK_BUILD}/meson"
          "${DXVK_BUILD}/libvgd-dxvk-d3d11.a" ${DXVK_MESON_LIBS}
  BYPRODUCTS ${DXVK_LIBS}
  COMMENT "Building DXVK (meson)"
  USES_TERMINAL)

# The libraries with the Windows libraries they need (as DXVK links d3d11.dll, less dxgi: vgd3d11 never creates
# DXGI factories, and MinGW's libdxgi.a would bind D3DKMT calls to its stubs), and DXVK's headers and build
# configuration, which vgd3d11's bridge to DXVK compiles against.
add_library(vgd-dxvk INTERFACE)
add_dependencies(vgd-dxvk dxvk-build)
target_link_libraries(vgd-dxvk INTERFACE
    -Wl,--start-group ${DXVK_LIBS} -Wl,--end-group
    gdi32 setupapi user32 ole32 oleaut32 uuid advapi32 shell32)
target_include_directories(vgd-dxvk SYSTEM INTERFACE
    "${DXVK_SRC}/include" "${DXVK_SRC}/include/vulkan/include" "${DXVK_SRC}/include/spirv/include"
    "${DXVK_BUILD}/meson")
# What DXVK's meson.build sets for every source on Windows.
target_compile_definitions(vgd-dxvk INTERFACE NOMINMAX _WIN32_WINNT=0xa00 DXVK_WSI_WIN32)
target_compile_options(vgd-dxvk INTERFACE -msse -msse2 -msse3 -mfpmath=sse)
