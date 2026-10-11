# The module "rpg", «RPG с видом сверху» (step 14.4a, 14.4-rpg-модель.md). Its rules of walking are a library of their
# own, with no window and no files: tests/test_rpg.cpp tests them alone, the module's game moves and draws by them.
add_library(forge_rpg_rules STATIC rpg_rules.cpp)
target_include_directories(forge_rpg_rules PUBLIC .)
target_link_libraries(forge_rpg_rules PUBLIC forge::core)
forge_target_defaults(forge_rpg_rules)

# What the game and the editor share of a map: the blocks' components, the map as drawn, how a map is drawn, the
# editor's level module.
add_library(forge_rpg_lib STATIC rpg_level.cpp)
target_include_directories(forge_rpg_lib PUBLIC .)
target_link_libraries(forge_rpg_lib PUBLIC forge_rpg_rules forge::level forge::objects forge::render forge::assets yyjson)
forge_target_defaults(forge_rpg_lib)

# The module "rpg" itself: its game, the maker of its test game and its forge::modules::ModuleDef with the self-test.
# What links it sees module/rpg_module.h only.
add_library(forge_rpg_module STATIC rpg_game.cpp rpg_testgame.cpp rpg_module.cpp)
target_include_directories(forge_rpg_module PUBLIC module)
target_link_libraries(forge_rpg_module PUBLIC forge::modules
                      PRIVATE forge_rpg_lib forge::game forge::editor forge::logic forge::audio forge::platform RmlUi::Core yyjson)
# Running from the build folder reads the UI, the module's files and its test game from the source tree.
target_compile_definitions(forge_rpg_module PRIVATE FORGE_UI_DIR="${PROJECT_SOURCE_DIR}/ui"
                                                    RPG_GAME_DIR="${PROJECT_SOURCE_DIR}/tests/data/rpg/game"
                                                    RPG_MODULE_DIR="${PROJECT_SOURCE_DIR}/games/modules/rpg"
                                                    RPG_GAMES_DIR="${PROJECT_SOURCE_DIR}/games")
forge_target_defaults(forge_rpg_module)
