add_library(forge_logic STATIC src/data.cpp src/words.cpp src/compile.cpp src/scheme.cpp src/runtime.cpp)
add_library(forge::logic ALIAS forge_logic)
target_include_directories(forge_logic PUBLIC include)
target_link_libraries(forge_logic PUBLIC forge::core forge::script forge::objects forge::scene forge::sim
  PRIVATE yyjson Luau.VM)
forge_embed_text(forge_logic nodes/logic.nodes.json forge_logic_nodes)
forge_target_defaults(forge_logic)
