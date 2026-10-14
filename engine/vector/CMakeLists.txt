# Vector shapes without drawing: paths read from SVG, the morph, easing (step 1 of docs/models/вектор-модель.md), the
# outline of an SVG file (step 3).
# Only the core: ThorVG and lunasvg draw what comes out, each in its own place.
add_library(forge_vector STATIC
  src/trig.cpp
  src/ease.cpp
  src/path.cpp
  src/morph.cpp
  src/svg.cpp)
add_library(forge::vector ALIAS forge_vector)
target_include_directories(forge_vector PUBLIC include)
target_link_libraries(forge_vector PUBLIC forge::core)
forge_target_defaults(forge_vector)
