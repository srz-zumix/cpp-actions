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
  unit/cleanup_test.cpp
  unit/config_test.cpp
  unit/action_test.cpp
  unit/node_provider_test.cpp
  unit/builtin_test.cpp
  unit/action_runner_test.cpp
  unit/sandbox_test.cpp
  unit/proxy_test.cpp
  unit/builtin_sandbox_test.cpp
  unit/commands_test.cpp
  unit/engine_test.cpp
  unit/hosted_python_test.cpp
  unit/reusable_workflow_test.cpp
  unit/snapshot_test.cpp
)
target_link_libraries(cpa_unit_tests PRIVATE cpa_core cpa_iutest)
if(APPLE)
  target_compile_definitions(cpa_unit_tests PRIVATE CPA_CMAKE_COMMAND="${CMAKE_COMMAND}")
  add_executable(cpa_msbuild_socket_probe helpers/msbuild_socket_probe.cpp)
  set(MSBUILD_SOCKET_PROBE "$<TARGET_FILE:cpa_msbuild_socket_probe>")
  target_compile_definitions(cpa_unit_tests PRIVATE CPA_MSBUILD_SOCKET_PROBE="${MSBUILD_SOCKET_PROBE}")
  add_dependencies(cpa_unit_tests cpa_msbuild_socket_probe)
  add_executable(cpa_apple_security_probe helpers/apple_security_probe.cpp)
  set(APPLE_SECURITY_PROBE "$<TARGET_FILE:cpa_apple_security_probe>")
  target_compile_definitions(cpa_unit_tests PRIVATE CPA_APPLE_SECURITY_PROBE="${APPLE_SECURITY_PROBE}")
  add_dependencies(cpa_unit_tests cpa_apple_security_probe)
  add_executable(cpa_home_probe helpers/home_probe.m)
  find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
  target_link_libraries(cpa_home_probe PRIVATE "${FOUNDATION_FRAMEWORK}")
  set(HOME_PROBE "$<TARGET_FILE:cpa_home_probe>")
  target_compile_definitions(cpa_unit_tests PRIVATE CPA_HOME_PROBE="${HOME_PROBE}")
  add_dependencies(cpa_unit_tests cpa_home_probe)
endif()
add_test(NAME unit COMMAND cpa_unit_tests)
add_test(NAME cli COMMAND ${CMAKE_COMMAND}
  -DCLI=$<TARGET_FILE:cpp-actions>
  -DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli-test
  -DHOST_IS_APPLE=${APPLE}
  -DAPPLE_SECURITY_PROBE=${APPLE_SECURITY_PROBE}
  -DMSBUILD_SOCKET_PROBE=${MSBUILD_SOCKET_PROBE}
  -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_test.cmake)
set_tests_properties(cli PROPERTIES TIMEOUT 120)
add_test(NAME cli_run COMMAND ${CMAKE_COMMAND}
  -DCPP_ACTIONS=$<TARGET_FILE:cpp-actions>
  -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/cli-run
  -P ${CMAKE_CURRENT_SOURCE_DIR}/integration/run_test.cmake)
set_tests_properties(cli_run PROPERTIES TIMEOUT 120)
add_test(NAME cli_actions COMMAND ${CMAKE_COMMAND}
  -DCLI=$<TARGET_FILE:cpp-actions>
  -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/cli-actions
  -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/run.cmake)
set_tests_properties(cli_actions PROPERTIES TIMEOUT 120)
add_test(NAME cli_config COMMAND ${CMAKE_COMMAND}
  -DCLI=$<TARGET_FILE:cpp-actions>
  -DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli-config
  -DHOST_IS_APPLE=${APPLE}
  -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/config.cmake)
set_tests_properties(cli_config PROPERTIES TIMEOUT 120)

find_program(CPA_BASH bash)
if(CPA_BASH)
  add_test(NAME hosted_python_script COMMAND "${CPA_BASH}"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/install_hosted_python_test.sh"
    "${PROJECT_SOURCE_DIR}/scripts/install-hosted-python.sh")
  set_tests_properties(hosted_python_script PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
  find_program(CPA_PYTHON3 python3)
  if(CPA_PYTHON3)
    find_program(CPA_RUBY ruby)
    if(NOT CPA_RUBY)
      message(STATUS "Ruby is unavailable; release workflow YAML tests are skipped")
    endif()
    add_test(NAME release_publish_script COMMAND "${CPA_PYTHON3}"
      "${CMAKE_CURRENT_SOURCE_DIR}/scripts/publish_release_test.py"
      "${PROJECT_SOURCE_DIR}/scripts/publish-release.sh")
    set_tests_properties(release_publish_script PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
    add_test(NAME release_version_script COMMAND "${CPA_PYTHON3}"
      "${CMAKE_CURRENT_SOURCE_DIR}/scripts/release_version_test.py"
      "${PROJECT_SOURCE_DIR}/scripts/release-version.py")
    set_tests_properties(release_version_script PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
  else()
    message(STATUS "Python 3 is unavailable; release script tests are skipped")
  endif()
else()
  message(STATUS "Bash is unavailable; shell script tests are skipped")
endif()
