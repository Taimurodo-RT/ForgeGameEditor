add_library(forge_render STATIC
  src/camera.cpp
  src/gpu.cpp
  src/lighting.cpp
  src/offscreen.cpp
  src/particles.cpp
  src/sprite_batch.cpp
  src/sprite_renderer.cpp
  src/tilemap_renderer.cpp)
add_library(forge::render ALIAS forge_render)
target_include_directories(forge_render PUBLIC include)
target_link_libraries(forge_render PUBLIC forge::core forge::world SDL3::SDL3-static PRIVATE stb_headers)
forge_add_shaders(forge_render
  shaders/tilemap.vert
  shaders/tilemap.frag
  shaders/sprite.vert
  shaders/sprite.frag
  shaders/particle.vert
  shaders/particles_emit.comp
  shaders/particles_update.comp
  shaders/light_spread.comp
  shaders/light_composite.vert
  shaders/light_composite.frag)
forge_target_defaults(forge_render)
