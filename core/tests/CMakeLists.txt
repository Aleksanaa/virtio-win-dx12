# SPDX-License-Identifier: MIT
foreach(t copy_path formats root_signature pipeline triangle descriptors depth_stencil)
  add_executable(${t} ${t}.cpp)
  target_link_libraries(${t} PRIVATE vgd-core ${CMAKE_DL_LIBS})
  target_include_directories(${t} PRIVATE ${CMAKE_SOURCE_DIR}/tests/shaders)
  add_test(NAME ${t} COMMAND ${t})
  set_tests_properties(${t} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()
