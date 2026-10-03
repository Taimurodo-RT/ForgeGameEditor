add_executable(forge_tests
  test_main.cpp
  test_memory.cpp
  test_handle.cpp
  test_jobs.cpp
  test_types.cpp
  test_reflect.cpp
  test_json.cpp
  test_binary.cpp
  test_assets.cpp
  test_world.cpp
  test_scene.cpp
  test_render.cpp
  test_sim.cpp
  test_cells.cpp
  test_factory.cpp
  test_time.cpp
  test_script.cpp
  test_graph.cpp)
target_link_libraries(forge_tests PRIVATE forge::core forge::data forge::assets forge::world forge::scene forge::sim forge::script forge::render doctest::doctest)
target_compile_definitions(forge_tests PRIVATE FORGE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME forge_tests COMMAND forge_tests)
