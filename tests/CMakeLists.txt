add_executable(forge_tests
  test_main.cpp
  test_memory.cpp
  test_handle.cpp
  test_jobs.cpp
  test_types.cpp
  test_reflect.cpp
  test_json.cpp
  test_binary.cpp)
target_link_libraries(forge_tests PRIVATE forge::core forge::data doctest::doctest)
add_test(NAME forge_tests COMMAND forge_tests)
