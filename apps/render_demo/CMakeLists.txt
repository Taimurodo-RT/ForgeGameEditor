add_executable(forge_render_demo main.cpp)
target_link_libraries(forge_render_demo PRIVATE forge::platform forge::render forge::world forge_demo_common)
forge_target_defaults(forge_render_demo)
