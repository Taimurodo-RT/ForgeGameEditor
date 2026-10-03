add_library(forge_core STATIC
  src/log.cpp
  src/memory.cpp
  src/jobs.cpp
  src/time.cpp
  src/file.cpp)
add_library(forge::core ALIAS forge_core)
target_include_directories(forge_core PUBLIC include)
target_link_libraries(forge_core PUBLIC Tracy::TracyClient)
find_package(Threads REQUIRED)
target_link_libraries(forge_core PUBLIC Threads::Threads)
forge_target_defaults(forge_core)
