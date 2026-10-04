add_library(forge_level STATIC
  src/level.cpp
  src/tile_edit.cpp)
add_library(forge::level ALIAS forge_level)
target_include_directories(forge_level PUBLIC include)
target_link_libraries(forge_level PUBLIC forge::core forge::world forge::scene forge::editor forge::render)
forge_target_defaults(forge_level)
