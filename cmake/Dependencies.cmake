# Third-party dependencies.
#
# Each dependency is first looked up with find_package() (e.g. provided by the
# vcpkg manifest in vcpkg.json or by Homebrew) and is downloaded with
# FetchContent only when it cannot be found.

include(FetchContent)

set(FETCHCONTENT_QUIET ON)

# yaml-cpp
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
set(YAML_CPP_FORMAT_SOURCE OFF CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(yaml-cpp
  URL https://github.com/jbeder/yaml-cpp/archive/refs/tags/yaml-cpp-0.9.0.tar.gz
  URL_HASH SHA256=25cb043240f828a8c51beb830569634bc7ac603978e0f69d6b63558dadefd49a
  DOWNLOAD_EXTRACT_TIMESTAMP ON
  FIND_PACKAGE_ARGS CONFIG)

# nlohmann/json
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
  DOWNLOAD_EXTRACT_TIMESTAMP ON
  FIND_PACKAGE_ARGS CONFIG)

# CLI11
set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(CLI11_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(CLI11
  URL https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.7.2.tar.gz
  URL_HASH SHA256=46eef3101da70852ec7af026e09d485ccee81813331c8c6052d39344443b83da
  DOWNLOAD_EXTRACT_TIMESTAMP ON
  FIND_PACKAGE_ARGS CONFIG)

FetchContent_MakeAvailable(yaml-cpp nlohmann_json CLI11)

if(NOT TARGET yaml-cpp::yaml-cpp AND TARGET yaml-cpp)
  add_library(yaml-cpp::yaml-cpp ALIAS yaml-cpp)
endif()

# iutest (header only, tests only)
#
# iutest is not available in the vcpkg registry, so it is taken from
# IUTEST_ROOT (environment or cache variable) when set, otherwise downloaded.
if(CPA_BUILD_TESTS)
  set(IUTEST_ROOT "$ENV{IUTEST_ROOT}" CACHE PATH "Path to an iutest checkout (optional)")
  if(IUTEST_ROOT AND EXISTS "${IUTEST_ROOT}/include/iutest.hpp")
    set(CPA_IUTEST_INCLUDE_DIR "${IUTEST_ROOT}/include")
  else()
    # SOURCE_SUBDIR points to a directory without CMakeLists.txt so that only
    # the sources are populated (iutest's own CMake project builds its tests
    # and samples and requires an old cmake_minimum_required()).
    FetchContent_Declare(iutest
      URL https://github.com/srz-zumix/iutest/archive/refs/tags/v1.17.1.tar.gz
      URL_HASH SHA256=8e2cb3548282cf4ea33e4ac68f981c904fc77405e9ec5715e22596c145a490a7
      DOWNLOAD_EXTRACT_TIMESTAMP ON
      SOURCE_SUBDIR include)
    FetchContent_MakeAvailable(iutest)
    set(CPA_IUTEST_INCLUDE_DIR "${iutest_SOURCE_DIR}/include")
  endif()
  add_library(cpa_iutest INTERFACE)
  target_include_directories(cpa_iutest SYSTEM INTERFACE "${CPA_IUTEST_INCLUDE_DIR}")
  message(STATUS "iutest: ${CPA_IUTEST_INCLUDE_DIR}")
endif()
