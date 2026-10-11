# The module "match3", «Три в ряд» (step 14.3b, 14.3-три-в-ряд-модель.md). Its rules of the board are a library of
# their own, with no window and no files: tests/test_match3.cpp tests them alone, the module's game animates them.
add_library(forge_match3_rules STATIC match3_rules.cpp)
target_include_directories(forge_match3_rules PUBLIC .)
target_link_libraries(forge_match3_rules PUBLIC forge::core)
forge_target_defaults(forge_match3_rules)
