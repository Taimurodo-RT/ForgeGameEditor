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

# libwebp: WebP pictures (stb does not read them). Library only, no tools.
foreach(opt ANIM_UTILS CWEBP DWEBP GIF2WEBP IMG2WEBP VWEBP WEBPINFO WEBPMUX EXTRAS)
  set(WEBP_BUILD_${opt} OFF CACHE BOOL "" FORCE)
endforeach()
set(WEBP_BUILD_LIBWEBPMUX OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libwebp
  GIT_REPOSITORY https://github.com/webmproject/libwebp.git
  GIT_TAG v1.6.0
  GIT_SHALLOW TRUE)

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

# FreeType: font rasterizer for the UI. Only the core is built: fonts ship as
# plain TTF files, so none of its optional codecs are needed.
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(freetype
  GIT_REPOSITORY https://github.com/freetype/freetype.git
  GIT_TAG VER-2-14-3
  GIT_SHALLOW TRUE)

# lunasvg: draws SVG pictures (icons, and the SVG data URIs web styles use) into textures for the UI.
set(LUNASVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(lunasvg
  GIT_REPOSITORY https://github.com/sammycage/lunasvg.git
  GIT_TAG v3.5.0
  GIT_SHALLOW TRUE)

# HarfBuzz: text shaping. The UI takes glyph widths and kerning from it, so text is as wide as in a browser.
# Only its single-file build (src/harfbuzz.cc) is compiled, with no optional backends.
FetchContent_Declare(harfbuzz
  GIT_REPOSITORY https://github.com/harfbuzz/harfbuzz.git
  GIT_TAG 14.5.1
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR do-not-configure)

# RmlUi: HTML/CSS-like documents for the editor and for game menus. Rendered
# by engine/ui on SDL_GPU. It lives in third_party/rmlui, where the engine
# extends it toward the web's styling.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
set(RMLUI_FONT_ENGINE "freetype" CACHE STRING "" FORCE)
set(RMLUI_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
# RmlUi looks for an installed FreeType; it gets the one fetched above instead.
set(CMAKE_DISABLE_FIND_PACKAGE_Freetype ON)
FetchContent_MakeAvailable(SDL3 tracy yyjson xxhash stb libwebp glslang spirv_cross flecs box2d luau freetype lunasvg harfbuzz)
add_library(harfbuzz STATIC ${harfbuzz_SOURCE_DIR}/src/harfbuzz.cc)
target_include_directories(harfbuzz SYSTEM PUBLIC ${harfbuzz_SOURCE_DIR}/src)
if(MSVC)
  target_compile_options(harfbuzz PRIVATE /bigobj)
endif()
if(NOT TARGET Freetype::Freetype)
  add_library(Freetype::Freetype ALIAS freetype)
endif()
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/rmlui ${CMAKE_BINARY_DIR}/third_party/rmlui)
target_link_libraries(rmlui_core PRIVATE harfbuzz)
# Engine code builds with strict warnings; RmlUi's headers are not ours to fix.
foreach(t rmlui_core rmlui_debugger)
  get_target_property(dirs ${t} INTERFACE_INCLUDE_DIRECTORIES)
  set_target_properties(${t} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${dirs}")
endforeach()

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
# libwebp's headers as <webp/decode.h>.
add_library(webp_codec INTERFACE)
target_link_libraries(webp_codec INTERFACE webpdemux webp)
target_include_directories(webp_codec SYSTEM INTERFACE ${libwebp_SOURCE_DIR}/src)

add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/sqlite ${CMAKE_BINARY_DIR}/third_party/sqlite)

if(FORGE_BUILD_TESTS)
  FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.5.3
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(doctest)
endif()
