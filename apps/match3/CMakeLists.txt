# The module "match3", «Три в ряд» (step 14.3b, 14.3-три-в-ряд-модель.md). Its rules of the board are a library of
# their own, with no window and no files: tests/test_match3.cpp tests them alone, the module's game animates them.
add_library(forge_match3_rules STATIC match3_rules.cpp)
target_include_directories(forge_match3_rules PUBLIC .)
target_link_libraries(forge_match3_rules PUBLIC forge::core)
forge_target_defaults(forge_match3_rules)

# What the game and the editor share of a level: the blocks' components, the set of gems, the field, how a board is
# drawn, the editor's level module.
add_library(forge_match3_lib STATIC match3_level.cpp)
target_include_directories(forge_match3_lib PUBLIC .)
target_link_libraries(forge_match3_lib PUBLIC forge_match3_rules forge::level forge::objects forge::render forge::assets yyjson)
forge_target_defaults(forge_match3_lib)

# The module "match3" itself: its game, the maker of its test game and its forge::modules::ModuleDef with the self-test.
# What links it sees module/match3_module.h only.
add_library(forge_match3_module STATIC match3_game.cpp match3_testgame.cpp match3_module.cpp)
target_include_directories(forge_match3_module PUBLIC module)
target_link_libraries(forge_match3_module PUBLIC forge::modules
                      PRIVATE forge_match3_lib forge::game forge::editor forge::logic forge::audio forge::platform RmlUi::Core yyjson)
# Running from the build folder reads the UI, the module's files and its test game from the source tree.
target_compile_definitions(forge_match3_module PRIVATE FORGE_UI_DIR="${PROJECT_SOURCE_DIR}/ui"
                                                       MATCH3_GAME_DIR="${PROJECT_SOURCE_DIR}/tests/data/match3/game"
                                                       MATCH3_MODULE_DIR="${PROJECT_SOURCE_DIR}/games/modules/match3"
                                                       MATCH3_GAMES_DIR="${PROJECT_SOURCE_DIR}/games")
forge_target_defaults(forge_match3_module)
