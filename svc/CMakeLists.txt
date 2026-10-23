# SPDX-License-Identifier: MIT
# yttrium's resolution service (vgpusrv.exe) and the helper it starts in each user session (viogpuap.exe), which
# applies the resolution the host asks for, so the guest follows the viewer window. Built from the vendored sources
# unchanged: compat/ covers what they take from MSVC, and the WDK headers MinGW lacks (d3dkmthk.h) come after
# MinGW's own, as for the UMDs.
set(viogpu ${CMAKE_SOURCE_DIR}/kmd/yttrium/viogpu)
set(sdk_inc "${VGD_WINKITS_DIR}/sdk/c/Include/10.0.26100.0")
set(svc_options -include ${CMAKE_CURRENT_SOURCE_DIR}/compat/msvc_compat.h
    "SHELL:-idirafter ${sdk_inc}/shared" "SHELL:-idirafter ${sdk_inc}/um")
set(svc_definitions UNICODE _UNICODE NDEBUG _CONSOLE)
set(svc_link_options -municode -static -static-libgcc -static-libstdc++)

add_executable(vgpusrv
    ${viogpu}/viogpusc/viogpusc.cpp ${viogpu}/viogpusc/Service.cpp ${viogpu}/viogpusc/Session.cpp
    ${viogpu}/viogpusc/SessionMgr.cpp ${viogpu}/viogpusc/utils.cpp ${viogpu}/shared/log.cpp ${viogpu}/shared/pipe.cpp)
target_include_directories(vgpusrv PRIVATE compat ${viogpu}/viogpusc ${viogpu}/shared)
target_compile_definitions(vgpusrv PRIVATE ${svc_definitions})
target_compile_options(vgpusrv PRIVATE ${svc_options})
target_link_libraries(vgpusrv PRIVATE wtsapi32 userenv advapi32)
target_link_options(vgpusrv PRIVATE ${svc_link_options})

add_executable(viogpuap
    ${viogpu}/viogpuap/viogpuap.cpp ${viogpu}/viogpuap/GpuAdapter.cpp ${viogpu}/viogpuap/GpuAdaptersMgr.cpp
    ${viogpu}/shared/log.cpp ${viogpu}/shared/pipe.cpp)
target_include_directories(viogpuap PRIVATE compat ${viogpu}/viogpuap ${viogpu}/shared)
target_compile_definitions(viogpuap PRIVATE ${svc_definitions})
target_compile_options(viogpuap PRIVATE ${svc_options})
target_link_libraries(viogpuap PRIVATE gdi32 user32 cfgmgr32 advapi32 wtsapi32 userenv)
target_link_options(viogpuap PRIVATE ${svc_link_options})
