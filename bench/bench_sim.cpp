// Simulation benchmark: 200 000 creatures with simple behaviour living in a
// side-view world, all of them awake (the "active objects" target):
//
//   logic     each creature walks, turns at random, jumps over walls
//   bodies    gravity + sliding along tiles for every one of them
//   triggers  2 000 zones reporting who enters and leaves
//   scene     re-filing everyone into chunks + spatial index
//   cells     liquids and falling sand (rain: 500 drops a frame all over)
//   rigid     Box2D crates and balls (4 000 dropped into the camera view)
//   rewind    recording the last 10 seconds around the camera (when on)
//
// Then the same world with the camera over one part of it: the rest is Near
// (simulated every 4th tick) or asleep, which is what a real game sees.
//
//   forge_bench_sim [--entities N] [--frames N] [--wells N] [--threads N]

#include "forge/core/jobs.h"
#include "forge/core/log.h"
#include "forge/core/time.h"
#include "forge/sim/simulation.h"
#include "forge/world/generators.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

struct BenchWalker {
    forge::f32 speed = 4;
    forge::f32 dir = 1;
    forge::u32 seed = 0;
};
FORGE_REFLECT_DECLARE(BenchWalker)
FORGE_REFLECT(BenchWalker, 1) {
    t.field("speed", &BenchWalker::speed);
    t.field("dir", &BenchWalker::dir);
    t.field("seed", &BenchWalker::seed);
}

using namespace forge;
using namespace forge::world;
using namespace forge::sim;
using scene::Position;

namespace {

struct Series {
    std::vector<f64> v;
    void print(const char* name) {
        std::sort(v.begin(), v.end());
        f64 sum = 0;
        for (f64 x : v) sum += x;
        FORGE_INFO("%-9s avg %7.3f ms   p99 %7.3f ms   worst %7.3f ms", name, sum / static_cast<f64>(v.size()),
                   v[v.size() * 99 / 100], v.back());
    }
};

u32 hash32(u32 a, u32 b) {
    u32 h = a * 374761393u + b * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

constexpr i32 kWidth = 16384; // tiles of world the creatures live in
const Rect kArea{0, -400, kWidth, 400};

void run_frames(Simulation& sim, const Rect& focus, u32 frames, bool all_awake, bool rain = false) {
    Series logic, bodies, triggers, scene, world, cells, rigid, rewind, total;
    u32 drop = 0;
    for (u32 f = 0; f < frames; ++f) {
        if (rain)
            for (u32 k = 0; k < 500; ++k, ++drop)
                sim.cells()->pour(focus.x0 + static_cast<i32>(hash32(drop, 21) % static_cast<u32>(focus.x1 - focus.x0)),
                                  focus.y0 + 1 + static_cast<i32>(hash32(drop, 22) % 64), 1, kFull / 2);
        const u64 t0 = time_now_ns();
        sim.update(1.0 / 60.0, focus);
        total.v.push_back(ns_to_ms(time_now_ns() - t0));
        const SimStats& s = sim.stats();
        logic.v.push_back(s.systems_ms);
        bodies.v.push_back(s.bodies_ms);
        triggers.v.push_back(s.triggers_ms);
        scene.v.push_back(s.scene_ms);
        world.v.push_back(s.world_ms);
        cells.v.push_back(s.cells_ms);
        rigid.v.push_back(s.rigid_ms);
        rewind.v.push_back(s.rewind_ms);
    }
    const SimStats& s = sim.stats();
    FORGE_INFO("%s: chunks active %u, near %u, asleep %u; bodies moved in the last tick %u; liquid chunks %u; "
               "rigid bodies %u (awake %u, tile chunks %u, solver %.2f ms)",
               rain ? "rain over everything" : (all_awake ? "everything awake" : "camera over one part"), s.zones.active,
               s.zones.near, s.zones.asleep, s.bodies_moved, s.cells.active_chunks, s.rigid.live, s.rigid.awake,
               s.rigid.tile_chunks, s.rigid.solver_ms);
    logic.print("logic");
    bodies.print("bodies");
    triggers.print("triggers");
    scene.print("scene");
    world.print("world");
    cells.print("cells");
    rigid.print("rigid");
    rewind.print("rewind");
    total.print("total");
}

} // namespace

int main(int argc, char** argv) {
    u32 entities = 200'000, frames = 300, wells = 50, threads = 0;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--entities") == 0) entities = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--frames") == 0) frames = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--threads") == 0) threads = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--wells") == 0) wells = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
    }
    jobs::init(threads);
    {
        WorldDesc wd;
        wd.layer_count = 3; // walls, blocks, liquids
        World world(wd, std::make_shared<SideViewGenerator>(7));
        scene::Scene scene(world);
        scene.register_component<BenchWalker>();
        SimDesc desc;
        desc.gravity_y = 40;
        desc.liquid_layer = 2;
        Simulation sim(world, scene, desc);
        for (TileId t : {TileGrass, TileDirt, TileStone, TileSand, TileCopper, TileIron, TileGold})
            sim.collision().set(t, TileShape::Solid);
        sim.cells()->add_liquid({});
        sim.cells()->set_falling(TileSand, true);

        // Loads start a batch at a time: repeat until the whole area is in.
        for (u32 last = ~0u; world.stats().resident != last;) {
            last = world.stats().resident;
            sim.update(0, kArea);
            world.finish_loading();
        }
        sim.update(0, kArea);

        // Creatures stand on the surface along the whole width.
        std::vector<i32> surface(kWidth);
        for (i32 x = 0; x < kWidth; ++x) {
            i32 y = kArea.y0;
            while (y < kArea.y1 && world.tile(1, x, y) == TileAir) ++y;
            surface[static_cast<usize>(x)] = y;
        }
        // Spawned chunk by chunk, as a world's populator does, so neighbours in
        // the world are mostly neighbours in memory too.
        std::vector<i32> xs(entities);
        for (u32 i = 0; i < entities; ++i) xs[i] = static_cast<i32>(hash32(i, 1) % kWidth);
        std::sort(xs.begin(), xs.end());
        for (u32 i = 0; i < entities; ++i) {
            const i32 x = xs[i];
            flecs::entity e = scene.spawn(Position::at_tile(x + 0.5, surface[static_cast<usize>(x)] - 1.0 - (hash32(i, 2) % 8)));
            if (!e.is_valid()) {
                FORGE_ERROR("creature %u: chunk not loaded at %d,%d (resident %u)", i, x, surface[static_cast<usize>(x)],
                            world.stats().resident);
                return 1;
            }
            Body b;
            b.half_w = 0.35f;
            b.half_h = 0.45f;
            e.set<Body>(b);
            e.set<BenchWalker>({2.0f + static_cast<f32>(hash32(i, 3) % 400) / 100.0f, (i & 1) ? 1.0f : -1.0f, i});
        }
        for (u32 i = 0; i < 2000; ++i) {
            const i32 x = static_cast<i32>(hash32(i, 9) % kWidth);
            scene.spawn(Position::at_tile(x + 0.5, surface[static_cast<usize>(x)] - 2.0)).set<Trigger>({3.0f});
        }
        for (u32 i = 0; i < wells; ++i) {
            const i32 x = static_cast<i32>(hash32(i, 11) % kWidth);
            GravitySource g;
            g.radius = 12;
            g.strength = 30;
            g.fade = true;
            scene.spawn(Position::at_tile(x + 0.5, surface[static_cast<usize>(x)] - 10.0)).set<GravitySource>(g);
        }

        auto walkers = scene.ecs().query<Position, Body, BenchWalker>();
        sim.add_system([&](const TickContext& ctx) {
            each_due(ctx, walkers, [&](flecs::entity_t, f32, Position&, Body& b, BenchWalker& w) {
                w.seed = hash32(w.seed, static_cast<u32>(ctx.tick));
                if ((w.seed & 255) == 0) w.dir = -w.dir; // now and then: turn around
                if (!(b.contacts & OnGround)) return;
                if (b.contacts & (w.dir > 0 ? HitRight : HitLeft)) {
                    // A wall ahead: jump against the pull (or turn if it is too high).
                    const f32 g = std::sqrt(b.gx * b.gx + b.gy * b.gy);
                    if (g > 0 && (w.seed & 3) != 0) {
                        b.vx -= b.gx / g * 14.0f;
                        b.vy -= b.gy / g * 14.0f;
                    } else {
                        w.dir = -w.dir;
                    }
                }
                b.vx = w.dir * w.speed;
            });
        });

        FORGE_INFO("%u creatures, 2000 triggers, %u gravity wells, %u threads", entities, wells, jobs::thread_count());
        run_frames(sim, kArea, 60, true); // settle: everyone lands
        run_frames(sim, kArea, frames, true);
        // A 1920×1080 view at 16 px per tile is 120 × 68 tiles.
        const Rect view{8000, -100, 8120, -32};
        run_frames(sim, view, frames, false);
        // A pile of 4 000 crates and balls in that view, falling and stacking.
        for (u32 i = 0; i < 4000; ++i) {
            const i32 x = 8004 + static_cast<i32>(i % 112);
            RigidBody rb;
            if (i % 3 == 0) {
                rb.shape = static_cast<u8>(RigidShape::Circle);
                rb.half_w = 0.45f;
            }
            scene.spawn(Position::at_tile(x + 0.5, surface[static_cast<usize>(x)] - 4.0 - (i / 112) * 1.2))
                .set<RigidBody>(rb);
        }
        // The same view while recording for rewinding.
        sim.rewind().set_recording(true);
        run_frames(sim, view, frames, false);
        FORGE_INFO("rewind: %u recordings, %.1f MB, %.1f s back", sim.rewind().stats().frames,
                   static_cast<f64>(sim.rewind().stats().bytes) / 1e6, sim.rewind().stats().seconds);
        sim.rewind().set_recording(false);
        run_frames(sim, view, 120, false); // settle
        FORGE_INFO("4000 rigid bodies in the view");
        run_frames(sim, view, frames, false);
        // Back over the whole stretch, with rain on all of it.
        const Rect sky{0, -400, kWidth, 400};
        for (u32 last = ~0u; world.stats().resident != last;) {
            last = world.stats().resident;
            sim.update(0, sky);
            world.finish_loading();
        }
        run_frames(sim, sky, frames, true, true);
    }
    jobs::shutdown();
    return 0;
}
