# The game's code as a library: the game itself links it, and so does the
# level editor, which paints «Старая шахта» through slice::SliceLevel.
add_library(forge_slice_lib STATIC slice_game.cpp slice_links.cpp slice_world.cpp slice_art.cpp slice_level.cpp slice_pictures.cpp slice_sounds.cpp
                            slice_edits.cpp)
target_include_directories(forge_slice_lib PUBLIC .)
target_link_libraries(forge_slice_lib PUBLIC forge::game forge::sim forge::render forge::ui forge::level forge::assets forge::audio forge::logic forge_demo_common)
forge_target_defaults(forge_slice_lib)

set(SLICE_SOURCES main.cpp)
if(WIN32)
  list(APPEND SLICE_SOURCES slice.rc) # the icon
endif()
add_executable(forge_slice ${SLICE_SOURCES})
target_link_libraries(forge_slice PRIVATE forge_slice_lib forge::platform)
# Running from the build folder reads the UI and the game's data from the
# source tree, so edits show up at once. A packaged game has them in "data".
target_compile_definitions(forge_slice PRIVATE FORGE_UI_DIR="${PROJECT_SOURCE_DIR}/ui"
                                               SLICE_DATA_DIR="${PROJECT_SOURCE_DIR}/games/slice")
# The screens of games/examples/simple-mode, as the editor's «Простой» makes them, built into the
# game for its self-test: they run in the game's own screens, packaged too.
file(GLOB SIMPLE_PAGES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/games/examples/simple-mode/*.html)
list(SORT SIMPLE_PAGES)
set(SIMPLE_PAGES_H "// Made by apps/slice/CMakeLists.txt from games/examples/simple-mode/*.html color-picker/*.html, key-timeline/*.html and layer-move/перенос_готово.html.\n")
string(APPEND SIMPLE_PAGES_H "struct SimplePage {\n    const char* name;\n    const char* html;\n};\n")
string(APPEND SIMPLE_PAGES_H "static const SimplePage kSimplePages[] = {\n")
foreach(page ${SIMPLE_PAGES})
  get_filename_component(page_name ${page} NAME_WE)
  file(READ ${page} page_html)
  string(APPEND SIMPLE_PAGES_H "    {\"${page_name}\", R\"forge_page(${page_html})forge_page\"},\n")
endforeach()
string(APPEND SIMPLE_PAGES_H "};\n")
# And the screen of games/examples/color-picker: colours with alpha as the colour picker leaves them.
file(GLOB COLOR_PAGES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/games/examples/color-picker/*.html)
list(SORT COLOR_PAGES)
string(APPEND SIMPLE_PAGES_H "static const SimplePage kColorPages[] = {\n")
foreach(page ${COLOR_PAGES})
  get_filename_component(page_name ${page} NAME_WE)
  file(READ ${page} page_html)
  string(APPEND SIMPLE_PAGES_H "    {\"${page_name}\", R\"forge_page(${page_html})forge_page\"},\n")
endforeach()
string(APPEND SIMPLE_PAGES_H "};\n")
# And the screen of games/examples/key-timeline: a coin moved by the author's keys.
file(GLOB TIMELINE_PAGES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/games/examples/key-timeline/*.html)
list(SORT TIMELINE_PAGES)
string(APPEND SIMPLE_PAGES_H "static const SimplePage kTimelinePages[] = {\n")
foreach(page ${TIMELINE_PAGES})
  get_filename_component(page_name ${page} NAME_WE)
  file(READ ${page} page_html)
  string(APPEND SIMPLE_PAGES_H "    {\"${page_name}\", R\"forge_page(${page_html})forge_page\"},\n")
endforeach()
string(APPEND SIMPLE_PAGES_H "};\n")
# And the finished screen of games/examples/layer-move: the button moved from frame A into frame B.
file(READ ${PROJECT_SOURCE_DIR}/games/examples/layer-move/перенос_готово.html page_html)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/games/examples/layer-move/перенос_готово.html)
string(APPEND SIMPLE_PAGES_H "static const SimplePage kMovePages[] = {\n    {\"перенос_готово\", R\"forge_page(${page_html})forge_page\"},\n};\n")
# And the screens of games/examples/screen-sound: a menu, a button over the game and a window, each with its music.
file(GLOB SOUND_PAGES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/games/examples/screen-sound/*.html)
list(SORT SOUND_PAGES)
string(APPEND SIMPLE_PAGES_H "static const SimplePage kSoundPages[] = {\n")
foreach(page ${SOUND_PAGES})
  get_filename_component(page_name ${page} NAME_WE)
  file(READ ${page} page_html)
  string(APPEND SIMPLE_PAGES_H "    {\"${page_name}\", R\"forge_page(${page_html})forge_page\"},\n")
endforeach()
string(APPEND SIMPLE_PAGES_H "};\n")
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/simple_pages.h CONTENT "${SIMPLE_PAGES_H}" @ONLY)
target_include_directories(forge_slice PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

# A game window without a console on Windows (log.txt in the save folder
# keeps the messages).
set_target_properties(forge_slice PROPERTIES WIN32_EXECUTABLE TRUE)
forge_target_defaults(forge_slice)

# The finished game: one folder with OldMine(.exe) and data/ next to it.
#   cmake --build build --target forge_slice_package [--config Release]
# puts it in build/dist/OldMine; copy that folder anywhere and run it.
install(FILES $<TARGET_FILE:forge_slice> DESTINATION . RENAME OldMine${CMAKE_EXECUTABLE_SUFFIX}
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
        COMPONENT slice)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/ui/forge-ui ${PROJECT_SOURCE_DIR}/ui/fonts ${PROJECT_SOURCE_DIR}/ui/game
                  ${PROJECT_SOURCE_DIR}/ui/slice ${PROJECT_SOURCE_DIR}/ui/web
        DESTINATION data/ui COMPONENT slice)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/games/slice/ DESTINATION data/game COMPONENT slice)
# The example of windows' behaviour (13.11), as the editor saved it: `OldMine --test --scene windows` reads its
# pages from data/examples/window-behaviour/ui, as the game reads its own game/ui.
install(DIRECTORY ${PROJECT_SOURCE_DIR}/games/examples/window-behaviour/ DESTINATION data/examples/window-behaviour COMPONENT slice)
# And the game made of templates (13.12) with its pictures, sound and own template: `OldMine --test --scene templates`.
install(DIRECTORY ${PROJECT_SOURCE_DIR}/games/examples/templates/ DESTINATION data/examples/templates COMPONENT slice)
# And the level made in the editor's «Физика» (step 9): `OldMine --test --scene physics` starts a new game from
# data/examples/physics/level, as «Играть отсюда» starts one from the editor's level folder.
install(DIRECTORY ${PROJECT_SOURCE_DIR}/games/examples/physics/ DESTINATION data/examples/physics COMPONENT slice)
# And the level made in the editor's «Свет» (step 9): `OldMine --test --scene light` plays it from
# data/examples/light/level and draws the editor's view of the same folder to compare their light.
install(DIRECTORY ${PROJECT_SOURCE_DIR}/games/examples/light/ DESTINATION data/examples/light COMPONENT slice)
# And the level made in the editor's «Зоны» (step 9) with its links and music: `OldMine --test --scene zones` plays
# it from data/examples/zones, its music from data/examples/zones/sounds.
install(DIRECTORY ${PROJECT_SOURCE_DIR}/games/examples/zones/ DESTINATION data/examples/zones COMPONENT slice)
# And the level the editor imported from a map of Tiled (step 10) with what the import wrote into the game (templates,
# pictures, music), its links and tmxrasterizer's frames of the map: `OldMine --test --scene tiled` plays it and
# compares. The map's own folder is the author's source, not the game's: the package has none of Tiled.
install(DIRECTORY ${PROJECT_SOURCE_DIR}/games/examples/tiled/ DESTINATION data/examples/tiled COMPONENT slice
        PATTERN "Карта Tiled" EXCLUDE)
add_custom_target(forge_slice_package
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/dist/OldMine
  COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR} --component slice --config $<CONFIG>
          --prefix ${CMAKE_BINARY_DIR}/dist/OldMine
  DEPENDS forge_slice
  COMMENT "Packaging the vertical slice into dist/OldMine"
  VERBATIM)
