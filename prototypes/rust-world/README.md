# Big-world check on Rust (prototype, not part of the Forge build)

The same scene as `forge_world_demo` on Rust: wgpu 30 for drawing, bevy_ecs 0.20 for creatures, rayon for chunk
generation. Generators, chunk size (64), streaming rules (margin 2, keep 2 more, 32 loads in flight per thread,
64 unloads per update), flight speed (3000 tiles/s at zoom 8) and the tile renderer design (chunk slots in one storage
buffer, one instance per chunk, tiles looked up per pixel, 192 uploads per frame) are ported one to one.

    cargo run --release -- --topdown --seconds 20            flight over the top-down world, then a summary
    cargo run --release -- --seconds 20                      side view (the C++ demo also draws lighting there)
    cargo run --release -- --topdown --dig --seconds 20      plus 64 tile edits a frame
    cargo run --release -- --crowd 200000 --particles 1000000 --seconds 20
    cargo run --release -- --headless 10 [--crowd N]         no window

Each timed run appends its summary to bench-results.txt. Esc quits, F toggles the flight, the wheel zooms.
For comparison: `forge_world_demo --topdown --fly --no-vsync` prints its frame and World::update times on exit.
