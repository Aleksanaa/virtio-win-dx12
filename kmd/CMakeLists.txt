# SPDX-License-Identifier: MIT
# yttrium's viogpu3d.sys (vendored in kmd/yttrium, with our changes) built with clang-cl and lld-link.
#
#   cmake -S kmd -B build/kmd -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/clang-cl-kmd.cmake && ninja -C build/kmd
#
# Differences from yttrium's MSBuild project, all build-side:
#   - WPP tracing is stubbed (its .tmh files come from a Windows-only preprocessor). The debug build logs through
#     DbgPrint/kdebugprint as before; only the WPP recorder (IFR) is gone.
#   - No version resource (yttrium's viogpu3d.rc needs MSBuild-generated vendor macros; not imported).
#   - clang does not provide MSVC's port I/O intrinsics out of line; shim/portio.c does.
cmake_minimum_required(VERSION 3.24)
project(vgd-kmd LANGUAGES C CXX)

get_filename_component(VGD_ROOT "${CMAKE_SOURCE_DIR}/.." ABSOLUTE)
set(VGD_WINKITS_DIR "${VGD_ROOT}/refs/winkits" CACHE PATH "WDK/SDK from the Microsoft.Windows.WDK.x64 / SDK.CPP NuGet packages")
set(VGD_KMD_SIGN_PFX "${CMAKE_SOURCE_DIR}/yttrium/build/VirtIOTestCert.pfx" CACHE FILEPATH
    "Test certificate for signing; yttrium's own, which guests running its test-signed builds already trust")

set(wdk_inc "${VGD_WINKITS_DIR}/wdk/c/Include/10.0.26100.0")
set(sdk_inc "${VGD_WINKITS_DIR}/sdk/c/Include/10.0.26100.0")
set(wdk_lib "${VGD_WINKITS_DIR}/wdk/c/Lib/10.0.26100.0/km/x64")
foreach(f "${wdk_inc}/km/ntddk.h" "${wdk_lib}/displib.lib")
  if(NOT EXISTS "${f}")
    message(FATAL_ERROR "${f} missing: extract the WDK NuGet package (c/Include and c/Lib/.../km/x64) into ${VGD_WINKITS_DIR}/wdk")
  endif()
endforeach()

set(src "${CMAKE_SOURCE_DIR}/yttrium")

# --- Shims ---------------------------------------------------------------------------------------------------------
set(gen "${CMAKE_BINARY_DIR}/gen")
set(sources
    viogpu/common/edid.cpp viogpu/common/viogpu_idr.cpp viogpu/common/viogpu_pci.cpp viogpu/common/viogpu_queue.cpp
    viogpu/common/baseobj.cpp viogpu/common/bitops.cpp
    viogpu/viogpu3d/driver.cpp viogpu/viogpu3d/viogpu_command.cpp viogpu/viogpu3d/viogpu_device.cpp
    viogpu/viogpu3d/viogpu_allocation.cpp viogpu/viogpu3d/viogpu_adapter.cpp viogpu/viogpu3d/viogpu_shmem_allocator.cpp
    viogpu/viogpu3d/viogpu_vidpn.cpp
    VirtIO/VirtIOPCICommon.c VirtIO/VirtIOPCILegacy.c VirtIO/VirtIOPCIModern.c VirtIO/VirtIORing-Packed.c
    VirtIO/VirtIORing.c)
foreach(s IN LISTS sources)
  get_filename_component(n "${s}" NAME_WE)
  if(NOT EXISTS "${gen}/${n}.tmh")
    file(WRITE "${gen}/${n}.tmh" "")
  endif()
endforeach()

# Headers are found case-sensitively here; Microsoft's and yttrium's sources spell some names differently from the
# files. casefix.sh links the requested spellings.
set(inc_dirs "${wdk_inc}/km/crt" "${wdk_inc}/km" "${wdk_inc}/shared" "${sdk_inc}/shared" "${sdk_inc}/um"
    "${src}/viogpu/common" "${src}/viogpu/shared" "${src}/VirtIO" "${src}/viogpu/viogpu3d")
execute_process(COMMAND sh "${CMAKE_SOURCE_DIR}/casefix.sh" "${gen}/casefix" ${inc_dirs} COMMAND_ERROR_IS_FATAL ANY)

# --- viogpu3d.sys --------------------------------------------------------------------------------------------------
list(TRANSFORM sources PREPEND "${src}/")
add_executable(viogpu3d ${sources} shim/portio.c)
set_target_properties(viogpu3d PROPERTIES SUFFIX ".sys" MSVC_RUNTIME_LIBRARY "")
# yttrium publishes debug builds (DBG=1: kdebugprint logging, assertions); we match that.
target_compile_definitions(viogpu3d PRIVATE
    _WIN64 _AMD64_ AMD64 _KERNEL_MODE WINNT=1 WINVER=0x0A00 _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A00000F
    POOL_NX_OPTIN=1 DEPRECATE_DDK_FUNCTIONS=1 DBG=1 VIOGPU_3D=1)
target_compile_options(viogpu3d PRIVATE
    -kernel /GS- /Gy /Z7 /O2 /W0 /Zc:wchar_t- -fms-compatibility -fms-extensions
    -Wno-error=incompatible-function-pointer-types
    "/FI${CMAKE_SOURCE_DIR}/shim/wpp_shim.h")
target_include_directories(viogpu3d PRIVATE "${gen}" "${src}/viogpu/common" "${src}/viogpu/shared" "${src}/VirtIO"
    "${src}/viogpu/viogpu3d")
foreach(d "${gen}/casefix" "${wdk_inc}/km/crt" "${wdk_inc}/km" "${wdk_inc}/shared" "${sdk_inc}/shared" "${sdk_inc}/um")
  target_compile_options(viogpu3d PRIVATE "SHELL:-imsvc ${d}")
endforeach()
# lld-link directly: CMake's MSVC link step adds a manifest, /INCREMENTAL and a console subsystem.
set(CMAKE_CXX_LINK_EXECUTABLE "<CMAKE_LINKER> /nologo <OBJECTS> /out:<TARGET> /pdb:<TARGET_PDB> <LINK_FLAGS> <LINK_LIBRARIES>")
set(CMAKE_EXE_LINKER_FLAGS "")
set(CMAKE_C_STANDARD_LIBRARIES "")
set(CMAKE_CXX_STANDARD_LIBRARIES "")
# GsDriverEntry (bufferoverflowfastfailk) sets up the stack cookie displib.lib is built with, then calls DriverEntry.
# Section flags as link.exe gives a driver: lld-link's /SECTION replaces the flags instead of adding to them, and a
# bare ",d" left INIT (which holds the entry point) neither readable nor executable; the kernel refused to load the
# image with STATUS_CONFLICTING_ADDRESSES. Non-PAGE sections are marked not paged.
target_link_options(viogpu3d PRIVATE
    /DRIVER /SUBSYSTEM:NATIVE,10.0 /ENTRY:GsDriverEntry /NODEFAULTLIB /DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF
    /TSAWARE:NO /MERGE:_TEXT=.text /MERGE:_PAGE=PAGE
    /SECTION:INIT,erd /SECTION:.text,erp /SECTION:.rdata,rp /SECTION:.data,rwp /SECTION:.pdata,rp)
target_link_libraries(viogpu3d PRIVATE
    "${wdk_lib}/bufferoverflowfastfailk.lib" "${wdk_lib}/ntoskrnl.lib" "${wdk_lib}/hal.lib" "${wdk_lib}/wmilib.lib"
    "${wdk_lib}/displib.lib")

# Test-signed copy, the file the guest loads.
find_program(OSSLSIGNCODE osslsigncode REQUIRED)
add_custom_command(TARGET viogpu3d POST_BUILD
    COMMAND "${OSSLSIGNCODE}" sign -pkcs12 "${VGD_KMD_SIGN_PFX}" -pass "" -h sha256
            -in "$<TARGET_FILE:viogpu3d>" -out "${CMAKE_BINARY_DIR}/signed/viogpu3d.sys.tmp" > /dev/null
    COMMAND "${CMAKE_COMMAND}" -E rename "${CMAKE_BINARY_DIR}/signed/viogpu3d.sys.tmp" "${CMAKE_BINARY_DIR}/signed/viogpu3d.sys"
    COMMENT "Test-signing viogpu3d.sys"
    VERBATIM)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/signed")
