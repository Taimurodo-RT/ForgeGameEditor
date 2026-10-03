add_library(forge_script STATIC
  src/host.cpp
  src/api_core.cpp
  src/nodes.cpp
  src/graph.cpp
  src/compiler.cpp)
add_library(forge::script ALIAS forge_script)
target_include_directories(forge_script PUBLIC include)
target_link_libraries(forge_script PUBLIC forge::core forge::data forge::scene forge::sim
  PRIVATE Luau.VM Luau.Compiler Luau.CodeGen)
forge_embed_text(forge_script nodes/std.nodes.json forge_std_nodes)
forge_target_defaults(forge_script)
