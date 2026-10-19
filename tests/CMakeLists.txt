add_executable(cpa_unit_tests
  unit/main.cpp
  unit/strings_test.cpp
  unit/value_test.cpp
  unit/expression_test.cpp
  unit/template_test.cpp
  unit/yaml_test.cpp
  unit/glob_test.cpp
  unit/sha256_test.cpp
  unit/process_test.cpp
  unit/parser_test.cpp
  unit/matrix_test.cpp
  unit/events_test.cpp
  unit/git_test.cpp
  unit/event_test.cpp
  unit/catalog_test.cpp
  unit/config_test.cpp
  unit/action_test.cpp
  unit/builtin_test.cpp
  unit/action_runner_test.cpp
)
target_link_libraries(cpa_unit_tests PRIVATE cpa_core cpa_iutest)
add_test(NAME unit COMMAND cpa_unit_tests)
add_test(NAME cli COMMAND ${CMAKE_COMMAND}
  -DCLI=$<TARGET_FILE:cpp-actions>
  -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/cli-test
  -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/run.cmake)
