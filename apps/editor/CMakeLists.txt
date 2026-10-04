add_executable(forge_editor_app main.cpp asset_library.cpp components.cpp dock_view.cpp file_types.cpp level_editor.cpp
                                object_library.cpp)
set_target_properties(forge_editor_app PROPERTIES OUTPUT_NAME forge_editor)
target_link_libraries(forge_editor_app PRIVATE forge::platform forge::render forge::ui forge::editor forge::assets forge::level
                      forge_slice_lib forge_demo_common)
# UI files are read from the source tree, so editing them updates the running editor.
# The level of «Старая шахта» lives next to the game's data; «Играть отсюда»
# starts the game built next to the editor.
target_compile_definitions(forge_editor_app PRIVATE FORGE_UI_DIR="${PROJECT_SOURCE_DIR}/ui"
                                                   SLICE_DATA_DIR="${PROJECT_SOURCE_DIR}/games/slice"
                                                   SLICE_LEVEL_DIR="${PROJECT_SOURCE_DIR}/games/slice/level"
                                                   FORGE_SLICE_EXE="$<TARGET_FILE:forge_slice>")
add_dependencies(forge_editor_app forge_slice)
forge_target_defaults(forge_editor_app)
