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
)
target_link_libraries(cpa_unit_tests PRIVATE cpa_core cpa_iutest)
add_test(NAME unit COMMAND cpa_unit_tests)
