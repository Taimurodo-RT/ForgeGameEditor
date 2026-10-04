add_library(forge_game STATIC
  src/vars.cpp
  src/dialogue.cpp
  src/quests.cpp
  src/saves.cpp
  src/shell.cpp
  src/runner.cpp)
add_library(forge::game ALIAS forge_game)
target_include_directories(forge_game PUBLIC include)
target_link_libraries(forge_game PUBLIC forge::core forge::data forge::platform forge::render forge::ui PRIVATE yyjson RmlUi::Core)
forge_target_defaults(forge_game)
