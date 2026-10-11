add_library(forge_modules STATIC
  src/modules.cpp)
add_library(forge::modules ALIAS forge_modules)
target_include_directories(forge_modules PUBLIC include)
target_link_libraries(forge_modules PUBLIC forge::core)
forge_target_defaults(forge_modules)
