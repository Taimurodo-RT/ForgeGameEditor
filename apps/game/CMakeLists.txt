# The launcher of Forge's games (step 14.3a, main.cpp): it reads the module a game names and runs that module's game.
# forge_launcher(<target> <name> [icon.rc]) builds it as <name> in the calling folder's build folder: forge_game here,
# and the same program under the old name forge_slice in build/apps/slice (apps/slice/CMakeLists.txt), which the
# package «Старая шахта» puts in as OldMine.
function(forge_launcher target name)
  set(sources ${PROJECT_SOURCE_DIR}/apps/game/main.cpp ${ARGN})
  add_executable(${target} ${sources})
  set_target_properties(${target} PROPERTIES OUTPUT_NAME ${name})
  target_link_libraries(${target} PRIVATE forge_builtin_modules forge::game forge::platform)
  # Running from the build folder with no --data: the game of the sources, «Старая шахта».
  target_compile_definitions(${target} PRIVATE FORGE_DEV_GAME_DIR="${PROJECT_SOURCE_DIR}/games/slice")
  # A game window without a console on Windows (log.txt in the save folder keeps the messages).
  set_target_properties(${target} PROPERTIES WIN32_EXECUTABLE TRUE)
  forge_target_defaults(${target})
endfunction()

forge_launcher(forge_game_app forge_game)
