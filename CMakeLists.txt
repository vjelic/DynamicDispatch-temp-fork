# Copyright (c) 2025 Advanced Micro Devices, Inc
#
# Permission is hereby granted, free of charge, to any person obtaining a copy of
# this software and associated documentation files (the "Software"), to deal in
# the Software without restriction, including without limitation the rights to
# use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
# the Software, and to permit persons to whom the Software is furnished to do so,
# subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
# FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
# COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
# IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
# CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

cmake_minimum_required(VERSION 3.21)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

include(ParseVersion)

# sets version_core and other variables
dd_parse_version(${CMAKE_CURRENT_SOURCE_DIR}/VERSION)
project(DynamicDispatch VERSION ${version_core} LANGUAGES C CXX)

if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)
  set(CMAKE_C_STANDARD 99)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED True)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  set(CMAKE_CXX_EXTENSION OFF)
  set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
endif()

set(DD_SRC_INCLUDE_DIRS
    ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/transaction/include
    ${PROJECT_SOURCE_DIR}/pm_package/include
    ${PROJECT_SOURCE_DIR}/xclbin/include ${PROJECT_SOURCE_DIR}/tests
)

if(MSVC)
  set(DD_DEFAULT_COMPILE_OPTIONS "/Zc:__cplusplus" "/W3" "/guard:cf")

  # set DEFAULT PARALLEL Level
  if(NOT DEFINED ENV{NUMBER_OF_PROCESSORS})
    set(CMAKE_BUILD_PARALLEL_LEVEL 8)
  else()
    set(CMAKE_BUILD_PARALLEL_LEVEL $ENV{NUMBER_OF_PROCESSORS})
  endif()
endif()

option(DD_DISABLE_AIEBU "Option to disable building aiebu" OFF)

include(FetchContent)
if(CMAKE_VERSION VERSION_GREATER "3.24")
  cmake_policy(SET CMP0135 NEW)
endif()

find_package(nlohmann_json REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(spdlog REQUIRED)
if(WIN32 AND (NOT DD_DISABLE_AIEBU))
  find_package(AIEBU REQUIRED)
endif()
find_package(xaiengine REQUIRED)
find_package(XRT REQUIRED)
include(zlib_dep) # find zlib

set(Protobuf_USE_STATIC_LIBS ON)
find_package(Protobuf CONFIG REQUIRED)
# NOTE: This workaround is required for DD to work locally in conda env.
# For more details, please see: https://github.com/protocolbuffers/protobuf/issues/11920
if(WIN32 AND Protobuf_VERSION VERSION_GREATER_EQUAL 4.22.0)
  find_package(absl REQUIRED)
  set(ABSL_LIB absl::abseil_dll)
endif()

option(UNIT_TEST_PERF_EN "Enable Performance checks in unit tests" OFF)
option(ENABLE_DD_TESTS "Enable tests" OFF)
option(ENABLE_SIMNOWLITE_BUILD "Build for linux simnowlite testing" OFF)
option(LOGGING_EN "Enable debug logging" ON)
option(PERF_LOGGING_EN "Enable performance logging" ON)
option(ENABLE_DD_PYTHON "Enable building the Python library" OFF)
option(XRT_RUNLIST_EN "Enable xrt experimental runlist feature" OFF)
option(DISABLE_LARGE_TXN_OPS "Disable large txn binaries in the package" OFF)

if(ENABLE_SIMNOWLITE_BUILD)
  add_definitions(-DSIMNOWLITE_EN)
  message("SIMNOWLITE_EN is defined.")
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "9.3")
      message(
        STATUS "CMAKE_CXX_COMPILER_VERSION: ${CMAKE_CXX_COMPILER_VERSION}"
      )
      message(FATAL_ERROR "\nTaskflow requires g++ at least v9.3")
    endif()
  endif()
endif()

if(LOGGING_EN)
  add_definitions(-DRYZENAI_TRACE)
  message("RYZENAI_TRACE is defined.")
endif()

if(DD_DISABLE_AIEBU)
  add_definitions(-DDD_DISABLE_AIEBU)
  message("DD_DISABLE_AIEBU is defined")
endif()

if(PERF_LOGGING_EN)
  add_definitions(-DRYZENAI_PERF)
  message("RYZENAI_PERF is defined.")
endif()

if(XRT_RUNLIST_EN)
  add_definitions(-DXRT_RUNLIST_EN)
  message("XRT_RUNLIST_EN is defined.")
endif()

# set the debug flag
if(CMAKE_BUILD_TYPE MATCHES "Debug")
  add_definitions(-DRYZENAI_DEBUG)
  message("RYZENAI_DEBUG is defined.")
endif()

add_subdirectory(src)
add_subdirectory(transaction)
add_subdirectory(pm_package)
add_subdirectory(xclbin)

if(ENABLE_DD_TESTS)
  add_subdirectory(tests)
endif()
