add_executable(forge_ui_demo main.cpp)
target_link_libraries(forge_ui_demo PRIVATE forge::platform forge::render forge::ui)
# UI files are read from the source tree, so editing them updates the running demo.
target_compile_definitions(forge_ui_demo PRIVATE FORGE_UI_DIR="${PROJECT_SOURCE_DIR}/ui")
forge_target_defaults(forge_ui_demo)
