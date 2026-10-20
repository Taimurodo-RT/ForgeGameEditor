# Vector shapes without drawing: paths read from SVG, the morph, easing (step 1 of docs/models/вектор-модель.md), the
# outline of an SVG file (step 3), the clips a game's screen plays (part 5в of docs/models/движение-модель.md) and the
# scenes a game plays (part 6г of docs/models/сцены-модель.md).
# Only the core and yyjson for those files: ThorVG and lunasvg draw what comes out, each in its own place.
add_library(forge_vector STATIC
  src/trig.cpp
  src/ease.cpp
  src/timing.cpp
  src/path.cpp
  src/morph.cpp
  src/svg.cpp
  src/svg_shape.cpp
  src/motion_file.cpp
  src/scene_file.cpp)
add_library(forge::vector ALIAS forge_vector)
target_include_directories(forge_vector PUBLIC include)
target_link_libraries(forge_vector PUBLIC forge::core PRIVATE yyjson)
forge_target_defaults(forge_vector)
