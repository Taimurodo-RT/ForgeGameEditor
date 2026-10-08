add_library(forge_editor STATIC
  src/document.cpp
  src/undo.cpp
  src/commands.cpp
  src/inspector.cpp
  src/dock.cpp
  src/ui_design.cpp
  src/ui_simple.cpp
  src/color_pick.cpp)
add_library(forge::editor ALIAS forge_editor)
target_include_directories(forge_editor PUBLIC include)
target_link_libraries(forge_editor PUBLIC forge::core forge::data PRIVATE yyjson)
forge_target_defaults(forge_editor)
