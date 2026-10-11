# The self-test's look into «Старая шахта» (slice_checks.cpp): the one file of the editor that sees slice's own types,
# as a library of its own that links slice PRIVATE, so no other file of the editor can include a header of slice
# (step 14.3a; tools/check_core_includes.py checks the sources the same way).
add_library(forge_editor_slice_checks STATIC slice_checks.cpp)
target_include_directories(forge_editor_slice_checks PUBLIC .)
target_link_libraries(forge_editor_slice_checks PUBLIC forge::level PRIVATE forge_slice_lib)
forge_target_defaults(forge_editor_slice_checks)

add_executable(forge_editor_app main.cpp asset_library.cpp components.cpp dock_view.cpp file_types.cpp level_editor.cpp level_tiled.cpp level_list.cpp
                                object_library.cpp logic_editor.cpp scheme_view.cpp story_editor.cpp converters.cpp
                                project_folder.cpp project_window.cpp ui_editor.cpp ui_editor_templates.cpp)
set_target_properties(forge_editor_app PROPERTIES OUTPUT_NAME forge_editor)
# The modules come from the composition root (apps/builtin): the editor names none of them.
target_link_libraries(forge_editor_app PRIVATE forge::platform forge::render forge::ui forge::editor forge::assets forge::audio forge::level forge::logic
                      forge::game forge::sim forge::objects forge::modules forge_builtin_modules forge_editor_slice_checks forge_demo_common
                      yyjson)
# UI files are read from the source tree, so editing them updates the running editor.
# The level of «Старая шахта» lives next to the game's data; «Играть отсюда»
# starts the game's launcher built next to the editor (forge_game: the module the game names).
target_compile_definitions(forge_editor_app PRIVATE FORGE_UI_DIR="${PROJECT_SOURCE_DIR}/ui"
                                                   SLICE_DATA_DIR="${PROJECT_SOURCE_DIR}/games/slice"
                                                   SLICE_LEVEL_DIR="${PROJECT_SOURCE_DIR}/games/slice/level"
                                                   FORGE_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/games/examples"
                                                   FORGE_GAMES_DIR="${PROJECT_SOURCE_DIR}/games"
                                                   FORGE_PROBE_GAME_DIR="${PROJECT_SOURCE_DIR}/tests/data/probe/game"
                                                   FORGE_EDITOR_EXE_NAME="$<TARGET_FILE_NAME:forge_editor_app>"
                                                   FORGE_GAME_EXE="$<TARGET_FILE:forge_game_app>"
                                                   FORGE_SLICE_EXE="$<TARGET_FILE:forge_slice>")
add_dependencies(forge_editor_app forge_game_app forge_slice)
forge_target_defaults(forge_editor_app)

# Converters («Конвертировать в…» in «Ресурсы») are Python scripts in
# converters/. On Windows the editor gets a Python of its own with their
# libraries (downloaded once, into the build folder); elsewhere it uses the
# system's python3. FORGE_PYTHON (an environment variable) overrides both.
if(WIN32)
  option(FORGE_BUNDLE_PYTHON "Put a Python with the converters' libraries next to the editor" ON)
else()
  option(FORGE_BUNDLE_PYTHON "Put a Python with the converters' libraries next to the editor" OFF)
endif()
set(FORGE_PYTHON_EXE "")
if(FORGE_BUNDLE_PYTHON)
  set(py_dir "${CMAKE_BINARY_DIR}/python")
  set(py_stamp "${py_dir}/forge.stamp")
  add_custom_command(OUTPUT "${py_stamp}"
                     COMMAND ${CMAKE_COMMAND} -DDIR=${py_dir} -DREQUIREMENTS=${PROJECT_SOURCE_DIR}/converters/requirements.txt
                             -DSTAMP=${py_stamp} -P ${PROJECT_SOURCE_DIR}/cmake/setup_python.cmake
                     DEPENDS ${PROJECT_SOURCE_DIR}/converters/requirements.txt ${PROJECT_SOURCE_DIR}/cmake/setup_python.cmake
                     COMMENT "Python для конвертеров")
  add_custom_target(forge_python ALL DEPENDS "${py_stamp}")
  add_dependencies(forge_editor_app forge_python)
  set(FORGE_PYTHON_EXE "${py_dir}/python.exe")
endif()
target_compile_definitions(forge_editor_app PRIVATE FORGE_CONVERTERS_DIR="${PROJECT_SOURCE_DIR}/converters"
                                                   FORGE_PYTHON_EXE="${FORGE_PYTHON_EXE}")
