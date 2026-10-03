add_library(forge_world STATIC
  src/world.cpp
  src/generators.cpp
  src/region_store.cpp)
add_library(forge::world ALIAS forge_world)
target_include_directories(forge_world PUBLIC include)
target_link_libraries(forge_world PUBLIC forge::core)
forge_target_defaults(forge_world)
