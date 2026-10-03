add_executable(forge_sim_demo main.cpp)
target_link_libraries(forge_sim_demo PRIVATE forge::platform forge::render forge::sim forge_demo_common)
forge_target_defaults(forge_sim_demo)
