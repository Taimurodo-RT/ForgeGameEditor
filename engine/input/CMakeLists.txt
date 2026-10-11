add_library(forge_input STATIC
  src/input.cpp)
add_library(forge::input ALIAS forge_input)
target_include_directories(forge_input PUBLIC include)
target_link_libraries(forge_input PUBLIC forge::core forge::data SDL3::SDL3-static PRIVATE yyjson)
forge_target_defaults(forge_input)
