# «Проба», the test module (step 14.3a): a second module beside «Старая шахта» for the tests of the launcher and the
# editor; built in only with --test and --self-test (apps/builtin). Its data is the tests': tests/data/probe.
add_library(forge_probe_module STATIC probe_module.cpp)
target_include_directories(forge_probe_module PUBLIC module)
target_link_libraries(forge_probe_module PUBLIC forge::modules PRIVATE forge::game forge::level forge::logic forge::objects forge::world yyjson)
target_compile_definitions(forge_probe_module PRIVATE FORGE_UI_DIR="${PROJECT_SOURCE_DIR}/ui"
                                                      PROBE_GAME_DIR="${PROJECT_SOURCE_DIR}/tests/data/probe/game"
                                                      PROBE_MODULE_DIR="${PROJECT_SOURCE_DIR}/tests/data/probe/module")
forge_target_defaults(forge_probe_module)
