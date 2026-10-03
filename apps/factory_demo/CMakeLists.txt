add_executable(forge_factory_demo main.cpp)
target_link_libraries(forge_factory_demo PRIVATE forge::platform forge::render forge::sim forge_demo_common)
forge_target_defaults(forge_factory_demo)
