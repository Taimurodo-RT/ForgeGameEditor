add_executable(forge_script_demo main.cpp)
target_link_libraries(forge_script_demo PRIVATE forge::platform forge::render forge::sim forge::script forge_demo_common)
# Graphs are read from the source tree, so editing them reloads the running demo.
target_compile_definitions(forge_script_demo PRIVATE FORGE_DEMO_GRAPHS="${CMAKE_CURRENT_SOURCE_DIR}/graphs")
forge_target_defaults(forge_script_demo)
