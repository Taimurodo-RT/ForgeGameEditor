# Forge on Rust

Forge is moving from C++ to Rust, module by module. Until everything is ported the C++ engine and editor in the rest
of this repository stay the working version; new work goes here, C++ only gets fixes. Project files (JSON, .html
screens, Luau) stay compatible, so a game made in either version opens in the other.

Stack: winit + wgpu for windows and drawing, bevy_ecs (as a library, not the Bevy framework) for entities, rayon for
worker threads; later Box2D v3, Luau (mlua), Blitz for UI, Vello, Kira. The motion editor Motif stays a separate
application with a bridge to Forge.

| Folder | What | Ported from |
|---|---|---|
| `crates/forge-world` | tile worlds in 64 × 64 chunks: sample generators, streaming around a focus | `engine/world` |
| `apps/world-demo` | the big-world demo: flight over a 64k × 64k world, 200k creatures, 1M particles | `apps/world_demo` |

    cargo test --workspace                         every test
    cargo run --release -p forge-world-demo        the demo (see apps/world-demo/README.md)

`crates/forge-world/tests/parity.rs` checks that the generators make the same tiles as C++: both it and
`tests/test_world.cpp` check the chunk hashes in `tests/data/world_chunks.txt`. Ported modules get such a check
wherever C++ and Rust must agree.

CI (`.github/workflows/ci.yml`, job `rust`) runs rustfmt, clippy with warnings as errors, the tests and a headless
demo on Windows and Linux.
