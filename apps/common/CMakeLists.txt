add_library(forge_demo_common STATIC demo_art.cpp project_edits.cpp)
target_include_directories(forge_demo_common PUBLIC .)
target_link_libraries(forge_demo_common PUBLIC forge::render forge::world forge::data)
forge_target_defaults(forge_demo_common)
