find_package(Threads REQUIRED)

add_library(cpa_core STATIC
  actions/action.cpp
  actions/builtin.cpp
  actions/fetch.cpp
  config/config.cpp
  engine/catalog.cpp
  engine/cleanup.cpp
  engine/commands.cpp
  engine/console.cpp
  engine/evaluate.cpp
  engine/job.cpp
  engine/event.cpp
  engine/masker.cpp
  engine/report.cpp
  engine/runner.cpp
  engine/shell.cpp
  util/fs.cpp
  util/glob.cpp
  util/process.cpp
  util/sha256.cpp
  util/strings.cpp
  expr/expression.cpp
  expr/template.cpp
  expr/value.cpp
  sandbox/hosted_python.cpp
  sandbox/profile.cpp
  sandbox/proxy.cpp
  sandbox/sandbox.cpp
  workflow/events.cpp
  workflow/matrix.cpp
  workflow/parser.cpp
  workflow/scopes.cpp
  workflow/yaml.cpp
  workspace/git.cpp
  workspace/snapshot.cpp
)
target_include_directories(cpa_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cpa_core
  PUBLIC cpa_options nlohmann_json::nlohmann_json Threads::Threads
  PRIVATE yaml-cpp::yaml-cpp)
target_compile_definitions(cpa_core PUBLIC CPA_VERSION="${PROJECT_VERSION}")

add_executable(cpp-actions cli/main.cpp)
target_link_libraries(cpp-actions PRIVATE cpa_core CLI11::CLI11)
