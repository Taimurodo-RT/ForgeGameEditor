add_library(forge_render STATIC
  src/gpu.cpp
  src/tilemap_renderer.cpp)
add_library(forge::render ALIAS forge_render)
target_include_directories(forge_render PUBLIC include)
target_link_libraries(forge_render PUBLIC forge::core forge::world SDL3::SDL3-static)
forge_add_shaders(forge_render
  shaders/tilemap.vert
  shaders/tilemap.frag)
forge_target_defaults(forge_render)
