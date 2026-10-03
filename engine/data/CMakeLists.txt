add_library(forge_data STATIC
  src/guid.cpp
  src/reflect.cpp
  src/json.cpp
  src/binary.cpp)
add_library(forge::data ALIAS forge_data)
target_include_directories(forge_data PUBLIC include)
target_link_libraries(forge_data PUBLIC forge::core PRIVATE yyjson)
forge_target_defaults(forge_data)
