# «Пиксельные» (step 2 of docs/models/вектор-модель.md): a vector character read from SVG, posed by a script and
# baked into pixel frames. ThorVG draws it, and only this library links ThorVG: games get the baked picture.
add_library(forge_editor_pixel STATIC
  src/svg.cpp
  src/rig.cpp
  src/script.cpp
  src/doc.cpp
  src/bake.cpp
  src/code_sites.cpp
  src/motion.cpp
  src/motion_edit.cpp
  src/motion_record.cpp)
target_include_directories(forge_editor_pixel PUBLIC include)
target_link_libraries(forge_editor_pixel PUBLIC forge::core forge::vector forge::assets PRIVATE thorvg Luau.VM Luau.Compiler Luau.Ast yyjson)
forge_target_defaults(forge_editor_pixel)
