add_executable(forge_bench_jobs bench_jobs.cpp)
target_link_libraries(forge_bench_jobs PRIVATE forge::core)
forge_target_defaults(forge_bench_jobs)

add_executable(forge_bench_data bench_data.cpp)
target_link_libraries(forge_bench_data PRIVATE forge::data)
forge_target_defaults(forge_bench_data)

add_executable(forge_bench_assets bench_assets.cpp)
target_link_libraries(forge_bench_assets PRIVATE forge::assets)
forge_target_defaults(forge_bench_assets)

add_executable(forge_bench_world bench_world.cpp)
target_link_libraries(forge_bench_world PRIVATE forge::world)
forge_target_defaults(forge_bench_world)

add_executable(forge_bench_scene bench_scene.cpp)
target_link_libraries(forge_bench_scene PRIVATE forge::scene)
forge_target_defaults(forge_bench_scene)

add_executable(forge_bench_sprites bench_sprites.cpp)
target_link_libraries(forge_bench_sprites PRIVATE forge::render)
forge_target_defaults(forge_bench_sprites)

add_executable(forge_bench_sim bench_sim.cpp)
target_link_libraries(forge_bench_sim PRIVATE forge::sim)
forge_target_defaults(forge_bench_sim)

add_executable(forge_bench_factory bench_factory.cpp)
target_link_libraries(forge_bench_factory PRIVATE forge::sim)
forge_target_defaults(forge_bench_factory)
