add_executable(forge_world_demo main.cpp)
target_link_libraries(forge_world_demo PRIVATE forge::platform forge::render forge::world stb_headers)
forge_target_defaults(forge_world_demo)
