add_executable(forge_bench_jobs bench_jobs.cpp)
target_link_libraries(forge_bench_jobs PRIVATE forge::core)
forge_target_defaults(forge_bench_jobs)

add_executable(forge_bench_data bench_data.cpp)
target_link_libraries(forge_bench_data PRIVATE forge::data)
forge_target_defaults(forge_bench_data)
