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
# Accept profiler connections from this machine only: no firewall prompt on Windows.
set(TRACY_ONLY_LOCALHOST ON CACHE BOOL "" FORCE)
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

# xxHash: content hashing for the asset pipeline (header-only use).
FetchContent_Declare(xxhash
  GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
  GIT_TAG v0.8.3
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR do-not-build)

# stb: image decoding for importers (header-only, pinned commit).
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20
  SOURCE_SUBDIR do-not-build)

# Shader toolchain, used only at build time by tools/shaderc: GLSL -> SPIR-V
# (glslang), SPIR-V -> HLSL / MSL (SPIRV-Cross). Shaders are written once in
# GLSL and compiled for every GPU backend while the engine builds.
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_PCH OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glslang
  GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
  GIT_TAG 16.6.0
  GIT_SHALLOW TRUE)

set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SHARED OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_STATIC ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS ON CACHE BOOL "" FORCE)
FetchContent_Declare(spirv_cross
  GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
  GIT_TAG vulkan-sdk-1.4.363.0
  GIT_SHALLOW TRUE)

# flecs: entity component system for everything that moves and acts.
set(FLECS_STATIC ON CACHE BOOL "" FORCE)
set(FLECS_SHARED OFF CACHE BOOL "" FORCE)
set(FLECS_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(flecs
  GIT_REPOSITORY https://github.com/SanderMertens/flecs.git
  GIT_TAG v4.1.6
  GIT_SHALLOW TRUE)

# Box2D v3: rigid bodies (crates, ragdolls, vehicles), multithreaded through
# the engine's job system.
set(BOX2D_SAMPLES OFF CACHE BOOL "" FORCE)
set(BOX2D_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BOX2D_BENCHMARKS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(box2d
  GIT_REPOSITORY https://github.com/erincatto/box2d.git
  GIT_TAG v3.1.1
  GIT_SHALLOW TRUE)

# Luau: the scripting language (sandboxed Lua from Roblox). Node graphs
# compile to it; only the compiler and the VM are built.
set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_WEB OFF CACHE BOOL "" FORCE)
set(LUAU_STATIC_CRT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(luau
  GIT_REPOSITORY https://github.com/luau-lang/luau.git
  GIT_TAG 0.741
  GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(SDL3 tracy yyjson xxhash stb glslang spirv_cross flecs box2d luau)

# Luau also defines its type checker and tools; the engine needs none of them.
foreach(t Luau.Analysis Luau.Config Luau.EqSat Luau.Require Luau.RequireNavigator Luau.CLI.lib Luau.Bytecode.Analysis)
  if(TARGET ${t})
    set_target_properties(${t} PROPERTIES EXCLUDE_FROM_ALL TRUE)
  endif()
endforeach()

add_library(xxhash_headers INTERFACE)
target_include_directories(xxhash_headers SYSTEM INTERFACE ${xxhash_SOURCE_DIR})
add_library(stb_headers INTERFACE)
target_include_directories(stb_headers SYSTEM INTERFACE ${stb_SOURCE_DIR})

add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/sqlite ${CMAKE_BINARY_DIR}/third_party/sqlite)

if(FORGE_BUILD_TESTS)
  FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.5.3
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(doctest)
endif()
