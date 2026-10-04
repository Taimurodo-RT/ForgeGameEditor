add_executable(forge_editor_app main.cpp components.cpp)
set_target_properties(forge_editor_app PROPERTIES OUTPUT_NAME forge_editor)
target_link_libraries(forge_editor_app PRIVATE forge::platform forge::render forge::ui forge::editor forge_demo_common)
# UI files are read from the source tree, so editing them updates the running editor.
target_compile_definitions(forge_editor_app PRIVATE FORGE_UI_DIR="${PROJECT_SOURCE_DIR}/ui")
forge_target_defaults(forge_editor_app)
