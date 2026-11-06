# SPDX-License-Identifier: MIT
# vgd-setup.exe, the installer: one executable with the driver package (pkg/vgd.inf and the files it lists) inside as
# a cabinet, which payload.cmake stamps and packs at each build. The KMD and the Mesa drivers come from their own
# builds (kmd/, icd/build.sh), which have to run first.
set(VGD_ICD_DIR "${CMAKE_SOURCE_DIR}/build/icd" CACHE PATH "icd/build.sh's output (vgd_vulkan_virtio.dll, viogpu_wgl.dll)")
set(VGD_KMD_SYS "${CMAKE_SOURCE_DIR}/build/kmd/signed/viogpu3d.sys" CACHE FILEPATH "The test-signed KMD (kmd/ build)")
find_program(GCAB gcab REQUIRED)
find_package(Git REQUIRED)

set(payload "${VGD_KMD_SYS}" "${VGD_ICD_DIR}/vgd_vulkan_virtio.dll" "${VGD_ICD_DIR}/viogpu_wgl.dll"
    "${CMAKE_SOURCE_DIR}/pkg/vgd_vulkan_virtio.json"
    $<TARGET_FILE:vgd3d11> $<TARGET_FILE:vgd3d12> $<TARGET_FILE:vgpusrv> $<TARGET_FILE:viogpuap>)
foreach(f "${VGD_KMD_SYS}" "${VGD_ICD_DIR}/vgd_vulkan_virtio.dll")
  if(NOT EXISTS "${f}")
    message(WARNING "${f} is missing: build the KMD and the ICD (README) before the installer")
  endif()
endforeach()
# A new commit changes the version.
set(head_log "${CMAKE_SOURCE_DIR}/.git/logs/HEAD")
if(NOT EXISTS "${head_log}")
  set(head_log "")
endif()

add_custom_command(
    OUTPUT payload.cab payload.rc version.txt
    # The dev shell sets SOURCE_DATE_EPOCH (1980), which string(TIMESTAMP) would take for the build time.
    COMMAND "${CMAKE_COMMAND}" -E env --unset=SOURCE_DATE_EPOCH "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}"
            "-DVERSION=${PROJECT_VERSION}" "-DGIT=${GIT_EXECUTABLE}" "-DGCAB=${GCAB}" "-DFILES=${payload}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/payload.cmake"
    DEPENDS payload.cmake "${CMAKE_SOURCE_DIR}/pkg/vgd.inf" ${payload} ${head_log}
    COMMENT "Packing the driver package"
    VERBATIM)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/payload.rc" PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/payload.cab")

enable_language(RC)
add_executable(vgd-setup WIN32 main.cpp system.cpp install.cpp ui.cpp setup.rc "${CMAKE_CURRENT_BINARY_DIR}/payload.rc")
target_compile_definitions(vgd-setup PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_link_libraries(vgd-setup PRIVATE setupapi newdev crypt32 cryptui wintrust ncrypt comctl32 shell32 shlwapi
    ole32 advapi32 ntdll)
target_link_options(vgd-setup PRIVATE -municode -s -static -static-libgcc -static-libstdc++)
