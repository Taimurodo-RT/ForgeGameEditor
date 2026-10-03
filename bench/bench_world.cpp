// World streaming benchmark: a camera flies across a 64k × 64k tile world at
// 60 frames per second and we measure what the player would feel.
//
//   main-thread cost of World::update per frame (must stay a small slice of 16.6 ms)
//   holes: visible chunks not ready yet when the frame is drawn
//   chunks generated per second and memory in use
//
//   forge_bench_world [--topdown] [--speed tiles_per_second] [--zoom px_per_tile] [--frames N]

#include "forge/core/jobs.h"
#include "forge/core/log.h"
#include "forge/core/memory.h"
#include "forge/core/time.h"
#include "forge/world/generators.h"
#include "forge/world/world.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

using namespace forge;
using namespace forge::world;

int main(int argc, char** argv) {
    bool top_down = false;
    f64 speed = 3000.0; // tiles per second: crossing 64k tiles in ~20 s
    f64 zoom = 1.0;     // pixels per tile: 1600×900 tiles on screen, the far zoom-out
    u32 frames = 1200;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--topdown") == 0) top_down = true;
        else if (std::strcmp(argv[i], "--speed") == 0 && i + 1 < argc) speed = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--zoom") == 0 && i + 1 < argc) zoom = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = static_cast<u32>(std::atoi(argv[++i]));
    }

    jobs::init();
    constexpr i32 kWorldChunks = 1024; // 64k × 64k tiles
    WorldDesc desc;
    desc.bounds = {0, 0, kWorldChunks, kWorldChunks};
    std::shared_ptr<const Generator> gen;
    if (top_down) gen = std::make_shared<TopDownGenerator>(2026);
    else gen = std::make_shared<SideViewGenerator>(2026, 2000); // surface 2000 tiles below the top
    {
        World w(desc, gen);

        const f64 view_w = 1600.0 / zoom, view_h = 900.0 / zoom;
        // Diagonal flight from near the top-left corner, through the surface
        // into the deep underground for the side view.
        f64 cx = 2000, cy = 1500;
        const f64 dir_x = 0.8, dir_y = 0.6;
        const f64 frame_s = 1.0 / 60.0;

        auto rect_at = [&](f64 x, f64 y) {
            return Rect{static_cast<i32>(x - view_w / 2), static_cast<i32>(y - view_h / 2),
                        static_cast<i32>(std::ceil(x + view_w / 2)), static_cast<i32>(std::ceil(y + view_h / 2))};
        };

        // Initial load, like a loading screen.
        const u64 load_start = time_now_ns();
        w.update(rect_at(cx, cy));
        w.finish_loading();
        for (int i = 0; i < 64 && w.stats().loading == 0; ++i) {
            const u32 before = w.stats().resident;
            w.update(rect_at(cx, cy));
            w.finish_loading();
            if (w.stats().resident == before) break;
        }
        const f64 load_ms = ns_to_ms(time_now_ns() - load_start);
        const u64 generated_at_start = w.stats().generated;

        std::vector<f64> update_ms;
        update_ms.reserve(frames);
        u64 visible_total = 0, holes_total = 0;
        u32 frames_with_holes = 0;
        u32 peak_resident = 0;
        const u64 start = time_now_ns();
        u64 deadline = start;
        for (u32 f = 0; f < frames; ++f) {
            cx += dir_x * speed * frame_s;
            cy += dir_y * speed * frame_s;
            const Rect view = rect_at(cx, cy);

            const u64 t0 = time_now_ns();
            w.update(view);
            update_ms.push_back(ns_to_ms(time_now_ns() - t0));

            // What the renderer would see this frame.
            const Rect vis = chunks_of(view).clipped(desc.bounds);
            u32 holes = 0;
            for (i32 y = vis.y0; y < vis.y1; ++y)
                for (i32 x = vis.x0; x < vis.x1; ++x) holes += w.find_chunk({x, y}) == nullptr;
            visible_total += static_cast<u64>((vis.x1 - vis.x0) * (vis.y1 - vis.y0));
            holes_total += holes;
            frames_with_holes += holes > 0;
            peak_resident = std::max(peak_resident, w.stats().resident);

            // Hold 60 FPS like a real game would, so streaming gets exactly the
            // time it would have.
            deadline += static_cast<u64>(frame_s * 1e9);
            const u64 now = time_now_ns();
            if (deadline > now) std::this_thread::sleep_for(std::chrono::nanoseconds(deadline - now));
        }
        const f64 seconds = ns_to_ms(time_now_ns() - start) / 1000.0;

        std::vector<f64> sorted = update_ms;
        std::sort(sorted.begin(), sorted.end());
        f64 sum = 0;
        for (f64 v : sorted) sum += v;
        const WorldStats s = w.stats();
        const u64 generated = s.generated - generated_at_start;
        const usize chunk_bytes = static_cast<usize>(desc.layer_count) * kChunkTiles * sizeof(TileId);

        FORGE_INFO("world 64k x 64k tiles, %s, %u threads", top_down ? "top down" : "side view", jobs::thread_count());
        FORGE_INFO("view %.0f x %.0f tiles, flight %.0f tiles/s for %.1f s", view_w, view_h, speed, seconds);
        FORGE_INFO("initial load: %.1f ms", load_ms);
        FORGE_INFO("World::update on main thread: avg %.3f ms, p99 %.3f ms, worst %.3f ms", sum / static_cast<f64>(sorted.size()),
                   sorted[sorted.size() * 99 / 100], sorted.back());
        FORGE_INFO("holes: %.3f%% of visible chunks, %u of %u frames had any", 100.0 * static_cast<f64>(holes_total) / static_cast<f64>(std::max<u64>(visible_total, 1)),
                   frames_with_holes, frames);
        FORGE_INFO("generated %llu chunks (%.0f per second), peak resident %u chunks = %.1f MiB",
                   static_cast<unsigned long long>(generated), static_cast<f64>(generated) / seconds, peak_resident,
                   static_cast<f64>(peak_resident) * static_cast<f64>(chunk_bytes) / static_cast<f64>(MiB));

        // Raw generation speed: all threads, no pacing.
        const u32 n = 4096;
        std::vector<TileId> scratch(static_cast<usize>(jobs::thread_count()) * desc.layer_count * kChunkTiles);
        const u64 g0 = time_now_ns();
        jobs::parallel_for(n, 16, [&](u32 b, u32 e) {
            const u32 t = jobs::this_thread_index();
            TileId* mine = scratch.data() + static_cast<usize>(t) * desc.layer_count * kChunkTiles;
            for (u32 i = b; i < e; ++i)
                gen->generate({static_cast<i32>(i % 64), static_cast<i32>(30 + i / 64)}, {mine, desc.layer_count});
        });
        const f64 gen_ms = ns_to_ms(time_now_ns() - g0);
        FORGE_INFO("raw generation: %u chunks in %.1f ms = %.0f chunks/s (%.1f us per chunk per thread)", n, gen_ms,
                   n / (gen_ms / 1000.0), gen_ms * 1000.0 * jobs::thread_count() / n);
    }
    jobs::shutdown();
    return 0;
}
