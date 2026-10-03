# Third-party code is fetched by exact tag so every machine builds the same thing.
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# SDL3: window, input, threads, GPU (Vulkan / D3D12 / Metal).
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG release-3.4.8
  GIT_SHALLOW TRUE)

# Tracy: CPU/GPU/memory profiler. Compiled out entirely when FORGE_PROFILE is OFF.
set(TRACY_ENABLE ${FORGE_PROFILE} CACHE BOOL "" FORCE)
set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
FetchContent_Declare(tracy
  GIT_REPOSITORY https://github.com/wolfpld/tracy.git
  GIT_TAG v0.14.1
  GIT_SHALLOW TRUE)

# yyjson: fast JSON reader/writer for project source files.
set(YYJSON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(yyjson
  GIT_REPOSITORY https://github.com/ibireme/yyjson.git
  GIT_TAG 0.13.0
  GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(SDL3 tracy yyjson)

if(FORGE_BUILD_TESTS)
  FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.5.3
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(doctest)
endif()
