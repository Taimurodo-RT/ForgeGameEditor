// Scene benchmark: 200 000 entities wandering around a loaded area of the
// world (the "active objects" target), measured per frame:
//
//   move      every entity moves (parallel over ECS tables)
//   index     Scene::update: chunk membership + spatial index for all of them
//   queries   10 000 "who is within 8 tiles" lookups (like AI looking around)
//
// then the cost of the player leaving (200 000 entities packed with their
// chunks), coming back (recreated) and saving.
//
//   forge_bench_scene [--entities N] [--frames N]

#include "forge/core/jobs.h"
#include "forge/core/log.h"
#include "forge/core/time.h"
#include "forge/scene/scene.h"
#include "forge/world/generators.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct BenchVelocity {
    forge::f32 x = 0;
    forge::f32 y = 0;
};
FORGE_REFLECT_DECLARE(BenchVelocity)
FORGE_REFLECT(BenchVelocity, 1) {
    t.field("x", &BenchVelocity::x);
    t.field("y", &BenchVelocity::y);
}

using namespace forge;
using namespace forge::world;
using namespace forge::scene;

namespace {

struct Timing {
    std::vector<f64> ms;
    void add(f64 v) { ms.push_back(v); }
    void print(const char* name) {
        std::sort(ms.begin(), ms.end());
        f64 sum = 0;
        for (f64 v : ms) sum += v;
        FORGE_INFO("%-8s avg %.3f ms, p99 %.3f ms, worst %.3f ms", name, sum / static_cast<f64>(ms.size()),
                   ms[ms.size() * 99 / 100], ms.back());
    }
};

// Moves every entity: the stand-in for game logic until scripts arrive.
void move_all(Scene& scene, flecs::query<Position, BenchVelocity>& q, f32 dt) {
    struct Span {
        Position* p;
        BenchVelocity* v;
        u32 n;
    };
    std::vector<Span> spans;
    q.run([&](flecs::iter& it) {
        while (it.next()) {
            Position* p = &it.field<Position>(0)[0];
            BenchVelocity* v = &it.field<BenchVelocity>(1)[0];
            const u32 n = static_cast<u32>(it.count());
            for (u32 b = 0; b < n; b += 4096) spans.push_back({p + b, v + b, std::min<u32>(4096, n - b)});
        }
    });
    jobs::parallel_for(static_cast<u32>(spans.size()), 1, [&](u32 b, u32 e) {
        for (u32 s = b; s < e; ++s)
            for (u32 i = 0; i < spans[s].n; ++i) {
                spans[s].p[i].x += spans[s].v[i].x * dt;
                spans[s].p[i].y += spans[s].v[i].y * dt;
            }
    });
    (void)scene;
}

} // namespace

int main(int argc, char** argv) {
    u32 target = 200'000;
    u32 frames = 300;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--entities") == 0) target = static_cast<u32>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--frames") == 0) frames = static_cast<u32>(std::atoi(argv[++i]));
    }

    jobs::init();
    {
        WorldDesc desc;
        desc.load_margin = 0;
        desc.keep_extra = 1;
        World world(desc, std::make_shared<TopDownGenerator>(11));
        Scene scene(world);
        scene.register_component<BenchVelocity>();
        const auto save_dir = std::filesystem::temp_directory_path() / ("forge_bench_scene_" + std::to_string(time_now_ns()));
        world.open_save(save_dir);
        scene.open_save(save_dir);

        // 32 × 32 chunks (2048 × 2048 tiles) loaded; entities spread evenly.
        const Rect home{0, 0, 2048, 2048};
        const u32 per_chunk = target / 1024;
        scene.set_populator([&](ChunkCoord c, Scene& s) {
            for (u32 i = 0; i < per_chunk; ++i) {
                const u32 h = hash_tile(5, c.x * 4096 + static_cast<i32>(i), c.y);
                const f32 x = static_cast<f32>(h % 6400) / 100.0f, y = static_cast<f32>((h >> 13) % 6400) / 100.0f;
                const f32 vx = static_cast<f32>(static_cast<i32>(h % 200) - 100) / 25.0f;
                const f32 vy = static_cast<f32>(static_cast<i32>((h >> 8) % 200) - 100) / 25.0f;
                s.spawn(Position{c.x, c.y, x, y}).set<BenchVelocity>({vx, vy});
            }
        });

        u64 t0 = time_now_ns();
        for (int i = 0; i < 50; ++i) {
            world.update(home);
            world.finish_loading();
        }
        scene.update();
        FORGE_INFO("populated %u chunks with %u entities in %.1f ms (%u threads)", scene.stats().chunks_indexed,
                   scene.stats().entities, ns_to_ms(time_now_ns() - t0), jobs::thread_count());

        auto movers = scene.ecs().query<Position, BenchVelocity>();
        Timing move, index, queries;
        std::atomic<u64> found{0};
        for (u32 f = 0; f < frames; ++f) {
            u64 a = time_now_ns();
            move_all(scene, movers, 1.0f / 60.0f);
            u64 b = time_now_ns();
            scene.update();
            u64 c = time_now_ns();
            jobs::parallel_for(10'000, 256, [&](u32 begin, u32 end) {
                std::vector<flecs::entity_t> out;
                u64 n = 0;
                for (u32 q = begin; q < end; ++q) {
                    const u32 h = hash_tile(f, static_cast<i32>(q), 0);
                    out.clear();
                    scene.query_radius(static_cast<f64>(h % 2048), static_cast<f64>((h >> 11) % 2048), 8.0f, out);
                    n += out.size();
                }
                found.fetch_add(n, std::memory_order_relaxed);
            });
            u64 d = time_now_ns();
            world.update(home);
            move.add(ns_to_ms(b - a));
            index.add(ns_to_ms(c - b));
            queries.add(ns_to_ms(d - c));
        }
        const SceneStats s = scene.stats();
        FORGE_INFO("%u entities still in the loaded area, %u walked out of it", s.entities, static_cast<u32>(s.packed));
        move.print("move");
        index.print("index");
        queries.print("queries");
        FORGE_INFO("queries found %.1f entities on average", static_cast<f64>(found.load()) / (10'000.0 * frames));

        // The player leaves: every chunk unloads and packs its entities.
        t0 = time_now_ns();
        const Rect away{1'000'000, 1'000'000, 1'000'064, 1'000'064};
        scene.update();
        world.update(away);
        world.finish_loading();
        const f64 leave_ms = ns_to_ms(time_now_ns() - t0);
        FORGE_INFO("leave: packed %llu entities in %.1f ms, %.1f MiB in memory", static_cast<unsigned long long>(scene.stats().packed),
                   leave_ms, static_cast<f64>(scene.stats().stored_bytes) / static_cast<f64>(MiB));

        t0 = time_now_ns();
        const SceneSaveReport save = scene.save();
        FORGE_INFO("save: %u chunks, %u region files, %.1f MiB in %.1f ms", save.chunks, save.regions,
                   static_cast<f64>(save.bytes) / static_cast<f64>(MiB), save.ms);

        // And comes back: entities are recreated from the save.
        t0 = time_now_ns();
        for (int i = 0; i < 50; ++i) {
            world.update(home);
            world.finish_loading();
        }
        scene.update();
        FORGE_INFO("return: %u entities back in %.1f ms", scene.stats().entities, ns_to_ms(time_now_ns() - t0));

        std::error_code ec;
        std::filesystem::remove_all(save_dir, ec);
    }
    jobs::shutdown();
    return 0;
}
