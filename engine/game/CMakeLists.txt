add_library(forge_game STATIC
  src/vars.cpp
  src/dialogue.cpp
  src/dialogue_source.cpp
  src/quests.cpp
  src/saves.cpp
  src/game_module.cpp
  src/shell.cpp
  src/runner.cpp
  src/screens.cpp)
add_library(forge::game ALIAS forge_game)
target_include_directories(forge_game PUBLIC include)
target_link_libraries(forge_game PUBLIC forge::core forge::data forge::platform forge::render forge::ui PRIVATE yyjson RmlUi::Core)
forge_target_defaults(forge_game)
