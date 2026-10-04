set(SLICE_SOURCES main.cpp slice_game.cpp slice_world.cpp slice_art.cpp)
if(WIN32)
  list(APPEND SLICE_SOURCES slice.rc) # the icon
endif()
add_executable(forge_slice ${SLICE_SOURCES})
target_link_libraries(forge_slice PRIVATE forge::game forge::sim forge::render forge::platform forge::ui forge_demo_common)
# Running from the build folder reads the UI and the game's data from the
# source tree, so edits show up at once. A packaged game has them in "data".
target_compile_definitions(forge_slice PRIVATE FORGE_UI_DIR="${PROJECT_SOURCE_DIR}/ui"
                                               SLICE_DATA_DIR="${PROJECT_SOURCE_DIR}/games/slice")
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
                  ${PROJECT_SOURCE_DIR}/ui/slice
        DESTINATION data/ui COMPONENT slice)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/games/slice/ DESTINATION data/game COMPONENT slice)
add_custom_target(forge_slice_package
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/dist/OldMine
  COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR} --component slice --config $<CONFIG>
          --prefix ${CMAKE_BINARY_DIR}/dist/OldMine
  DEPENDS forge_slice
  COMMENT "Packaging the vertical slice into dist/OldMine"
  VERBATIM)
