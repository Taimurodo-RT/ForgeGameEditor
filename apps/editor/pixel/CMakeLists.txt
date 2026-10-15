# «Векторная» (step 2 of docs/models/вектор-модель.md): a vector character read from SVG, posed by a script and
# baked into pixel frames. ThorVG draws it, and only this library links ThorVG: games get the baked picture.
add_library(forge_editor_pixel STATIC
  src/svg.cpp
  src/rig.cpp
  src/script.cpp
  src/doc.cpp
  src/draw.cpp
  src/bake.cpp
  src/clip_cut.cpp
  src/timeline.cpp
  src/motion.cpp
  src/motion_edit.cpp
  src/motion_record.cpp)
target_include_directories(forge_editor_pixel PUBLIC include)
target_link_libraries(forge_editor_pixel PUBLIC forge::core forge::vector forge::assets forge_editor_pixel_code PRIVATE thorvg Luau.VM Luau.Compiler yyjson)
forge_target_defaults(forge_editor_pixel)

# The place of every call of a character's code (code_sites.h) reads Luau's syntax tree through Luau's C++ headers, so
# it is built as Luau is (Warnings.cmake, WITH_EXCEPTIONS); its own header has none of Luau.
add_library(forge_editor_pixel_code STATIC src/code_sites.cpp)
target_include_directories(forge_editor_pixel_code PUBLIC include)
target_link_libraries(forge_editor_pixel_code PUBLIC forge::core PRIVATE Luau.Ast)
forge_target_defaults(forge_editor_pixel_code WITH_EXCEPTIONS)
