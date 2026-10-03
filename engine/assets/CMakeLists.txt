add_library(forge_assets STATIC
  src/content_hash.cpp
  src/asset_database.cpp
  src/asset_pipeline.cpp
  src/importers.cpp)
add_library(forge::assets ALIAS forge_assets)
target_include_directories(forge_assets PUBLIC include)
target_link_libraries(forge_assets PUBLIC forge::data PRIVATE sqlite3 xxhash_headers stb_headers)
forge_target_defaults(forge_assets)
