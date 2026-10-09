add_library(forge_level STATIC
  src/level.cpp
  src/tile_edit.cpp
  src/object_edit.cpp
  src/physics.cpp
  src/light.cpp
  src/areas.cpp
  src/own_tiles.cpp
  src/tiled.cpp
  src/tiled_import.cpp
  src/tiled_apply.cpp)
add_library(forge::level ALIAS forge_level)
target_include_directories(forge_level PUBLIC include)
target_link_libraries(forge_level PUBLIC forge::core forge::world forge::scene forge::objects forge::editor forge::render forge::data forge::sim PRIVATE forge::assets yyjson)
forge_target_defaults(forge_level)
