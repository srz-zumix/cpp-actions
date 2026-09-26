cmake_minimum_required(VERSION 3.24)

project(cpp-actions
  VERSION 0.1.0
  DESCRIPTION "Local GitHub Actions workflow emulator with a macOS Seatbelt sandbox"
  LANGUAGES CXX)

option(CPA_BUILD_TESTS "Build cpp-actions tests" ${PROJECT_IS_TOP_LEVEL})
option(CPA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(CPA_ENABLE_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Dependencies)

add_library(cpa_options INTERFACE)
target_compile_features(cpa_options INTERFACE cxx_std_20)
if(MSVC)
  target_compile_options(cpa_options INTERFACE /W4 /utf-8)
  if(CPA_WARNINGS_AS_ERRORS)
    target_compile_options(cpa_options INTERFACE /WX)
  endif()
else()
  target_compile_options(cpa_options INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)
  if(CPA_WARNINGS_AS_ERRORS)
    target_compile_options(cpa_options INTERFACE -Werror)
  endif()
  if(CPA_ENABLE_SANITIZERS)
    target_compile_options(cpa_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    target_link_options(cpa_options INTERFACE -fsanitize=address,undefined)
  endif()
endif()

add_subdirectory(src)

if(CPA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

include(GNUInstallDirs)
install(TARGETS cpp-actions RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
