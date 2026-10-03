add_library(forge_sim STATIC
  src/zones.cpp
  src/tiles.cpp
  src/bodies.cpp
  src/gravity.cpp
  src/cells.cpp
  src/rigid.cpp
  src/simulation.cpp)
add_library(forge::sim ALIAS forge_sim)
target_include_directories(forge_sim PUBLIC include)
target_link_libraries(forge_sim PUBLIC forge::core forge::data forge::world forge::scene PRIVATE box2d::box2d)
forge_target_defaults(forge_sim)
