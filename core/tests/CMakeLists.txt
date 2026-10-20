# SPDX-License-Identifier: MIT
foreach(t copy_path)
  add_executable(${t} ${t}.cpp)
  target_link_libraries(${t} PRIVATE vgd-core ${CMAKE_DL_LIBS})
  add_test(NAME ${t} COMMAND ${t})
  set_tests_properties(${t} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()
