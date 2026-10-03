cmake_minimum_required(VERSION 3.24)
project(Forge VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FORGE_PROFILE "Build with the Tracy profiler client" ON)
option(FORGE_BUILD_TESTS "Build unit tests" ON)
option(FORGE_BUILD_BENCH "Build benchmarks" ON)

include(cmake/Dependencies.cmake)
include(cmake/Warnings.cmake)

add_subdirectory(engine/core)
add_subdirectory(engine/data)
add_subdirectory(engine/platform)
add_subdirectory(apps/sandbox)

if(FORGE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(FORGE_BUILD_BENCH)
  add_subdirectory(bench)
endif()
