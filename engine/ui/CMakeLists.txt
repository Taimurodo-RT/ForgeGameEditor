add_library(forge_ui STATIC
  src/ui.cpp
  src/tokens.cpp
  src/interfaces.cpp
  src/html.cpp
  src/elements.cpp
  src/gpu_renderer.cpp)
add_library(forge::ui ALIAS forge_ui)
target_include_directories(forge_ui PUBLIC include)
target_link_libraries(forge_ui
  PUBLIC forge::core forge::render RmlUi::Core SDL3::SDL3-static
  PRIVATE RmlUi::Debugger yyjson stb_headers forge::assets lunasvg::lunasvg)
forge_add_shaders(forge_ui
  shaders/ui.vert
  shaders/ui_texture.frag
  shaders/ui_gradient.frag
  shaders/ui_fullscreen.vert
  shaders/ui_passthrough.frag
  shaders/ui_color_matrix.frag
  shaders/ui_blend_mask.frag
  shaders/ui_drop_shadow.frag
  shaders/ui_blur.vert
  shaders/ui_blur.frag)
forge_target_defaults(forge_ui)
