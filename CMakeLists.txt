# orderflow C++ core: ITCH 5.0 parser, L3 order book, benchmarks, Python bindings.
#
# Two ways in:
#   - Developers: `cmake --preset dev && cmake --build --preset dev && ctest --preset dev`
#     builds the library, the C++ tests, the tools and the benchmarks.
#   - pip: `pip install -e .` runs this file through scikit-build-core (SKBUILD is set),
#     which builds only the Python extension module.
cmake_minimum_required(VERSION 3.24)
project(orderflow LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)          # -std=c++20, not -std=gnu++20
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # compile_commands.json for editors and clang-tidy

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(ORDERFLOW_BUILD_TESTS "Build the C++ unit tests" ${PROJECT_IS_TOP_LEVEL})
option(ORDERFLOW_BUILD_TOOLS "Build the command-line tools" ${PROJECT_IS_TOP_LEVEL})
option(ORDERFLOW_BUILD_BENCH "Build the benchmarks" OFF)
option(ORDERFLOW_BUILD_PYTHON "Build the Python extension module" OFF)
option(ORDERFLOW_WITH_ONNXRUNTIME "Fetch ONNX Runtime and build bench_infer (184 MB download)" OFF)
set(ORDERFLOW_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. address,undefined or thread")
if(SKBUILD)
  # The pip build only needs the extension module; tests and benchmarks are dev-only.
  set(ORDERFLOW_BUILD_TESTS OFF)
  set(ORDERFLOW_BUILD_TOOLS OFF)
  set(ORDERFLOW_BUILD_BENCH OFF)
  set(ORDERFLOW_BUILD_PYTHON ON)
endif()

# Header-only library: the book and parser are templates, so they live in headers.
add_library(orderflow INTERFACE)
add_library(orderflow::orderflow ALIAS orderflow)
target_include_directories(orderflow INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/cpp/include)
target_compile_features(orderflow INTERFACE cxx_std_20)

# Warnings for our own targets only (not for fetched dependencies).
add_library(orderflow_warnings INTERFACE)
target_compile_options(orderflow_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast)
if(ORDERFLOW_WERROR)
  target_compile_options(orderflow_warnings INTERFACE -Werror)
endif()

if(ORDERFLOW_SANITIZE)
  # Applied globally so that dependencies (GoogleTest) are instrumented too:
  # mixing instrumented and uninstrumented code gives false reports under TSan.
  add_compile_options(-fsanitize=${ORDERFLOW_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${ORDERFLOW_SANITIZE})
endif()

include(FetchContent)

if(ORDERFLOW_BUILD_TESTS)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
    URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  enable_testing()
  add_subdirectory(cpp/tests)
endif()

if(ORDERFLOW_BUILD_TOOLS)
  add_subdirectory(cpp/src)
endif()

if(ORDERFLOW_BUILD_BENCH)
  FetchContent_Declare(benchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.4.tar.gz
    URL_HASH SHA256=b334658edd35efcf06a99d9be21e4e93e092bd5f95074c1673d5c8705d95c104
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)
  add_subdirectory(cpp/bench)
endif()

if(ORDERFLOW_WITH_ONNXRUNTIME)
  # Microsoft's NuGet package carries the C headers and the Linux x64 library.
  # CMake recognises archives only by extension, and a .nupkg is a zip.
  FetchContent_Declare(onnxruntime
    URL https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/1.32.0/microsoft.ml.onnxruntime.1.32.0.nupkg
    URL_HASH SHA256=a9150bc2992cbbb6722070da9910e1ffa989a8bfc67bdd5bb8e745ca755641db
    DOWNLOAD_NAME onnxruntime-1.32.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
  FetchContent_MakeAvailable(onnxruntime)
  set(ORT_LIB_DIR ${onnxruntime_SOURCE_DIR}/runtimes/linux-x64/native)
  # The library's SONAME is libonnxruntime.so.1 but the package ships it as
  # libonnxruntime.so, so give the loader the name it will look for.
  file(CREATE_LINK ${ORT_LIB_DIR}/libonnxruntime.so ${ORT_LIB_DIR}/libonnxruntime.so.1 SYMBOLIC)
  add_library(onnxruntime::onnxruntime SHARED IMPORTED)
  set_target_properties(onnxruntime::onnxruntime PROPERTIES
    IMPORTED_LOCATION ${ORT_LIB_DIR}/libonnxruntime.so
    IMPORTED_SONAME libonnxruntime.so.1
    INTERFACE_INCLUDE_DIRECTORIES ${onnxruntime_SOURCE_DIR}/build/native/include)
  add_executable(bench_infer cpp/bench/bench_infer.cpp)
  target_link_libraries(bench_infer PRIVATE orderflow orderflow_warnings onnxruntime::onnxruntime)
  set_target_properties(bench_infer PROPERTIES BUILD_RPATH ${ORT_LIB_DIR})
endif()

if(ORDERFLOW_BUILD_PYTHON)
  # pybind11 comes from the build environment (pyproject build-system.requires).
  find_package(Python REQUIRED COMPONENTS Interpreter Development.Module)
  find_package(pybind11 CONFIG REQUIRED)
  pybind11_add_module(_core cpp/bindings/module.cpp)
  target_link_libraries(_core PRIVATE orderflow orderflow_warnings)
  install(TARGETS _core LIBRARY DESTINATION orderflow)
endif()
