# SPDX-License-Identifier: MIT
# Third-party code built with its own meson build for the MinGW target (vkd3d-proton.cmake, dxvk.cmake). meson and
# ninja come from the dev shell. Each meson build is configured once in the build tree; every build runs its ninja,
# which does nothing when nothing changed.

find_program(VGD_MESON meson REQUIRED)
find_program(VGD_NINJA ninja REQUIRED)

# Writes a meson cross file for this build's MinGW compilers to `path`. GCC from nixpkgs uses the mcfgthread model,
# whose headers and library live outside the wrapper's paths. Written only when it changes: meson rebuilds
# everything when its cross file is touched.
function(vgd_meson_cross_file path)
  set(c_args "")
  set(link_args "")
  if(DEFINED ENV{VGD_MINGW_MCFG_INCLUDE})
    set(c_args "'-isystem', '$ENV{VGD_MINGW_MCFG_INCLUDE}'")
  endif()
  if(DEFINED ENV{VGD_MINGW_MCFG_LIB})
    set(link_args "'-L$ENV{VGD_MINGW_MCFG_LIB}'")
  endif()
  file(CONFIGURE OUTPUT "${path}" @ONLY CONTENT "\
[binaries]
c = '${CMAKE_C_COMPILER}'
cpp = '${CMAKE_CXX_COMPILER}'
ar = 'x86_64-w64-mingw32-ar'
strip = 'x86_64-w64-mingw32-strip'
windres = 'x86_64-w64-mingw32-windres'

[built-in options]
c_args = [@c_args@]
cpp_args = [@c_args@]
c_link_args = [@link_args@]
cpp_link_args = [@link_args@]

[properties]
needs_exe_wrapper = true

[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
")
endfunction()

# Configures the meson build of `src` in `build` (once) with the cross file at `cross` and extra options.
function(vgd_meson_setup name src build cross)
  if(NOT EXISTS "${build}/build.ninja")
    execute_process(
      COMMAND "${VGD_MESON}" setup "${build}" "${src}" --cross-file "${cross}" --buildtype=release -Db_ndebug=true
              ${ARGN}
      RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
      message(FATAL_ERROR "meson setup of ${name} failed")
    endif()
  endif()
endfunction()
