// Sprite batch benchmark, CPU only (no GPU needed): N sprites pushed from all
// threads, then culled, sorted into draw order and written out, per frame.
//
//   forge_bench_sprites [--sprites N] [--frames N]

#include "forge/core/jobs.h"
#include "forge/core/log.h"
#include "forge/core/time.h"
#include "forge/render/sprite_batch.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

using namespace forge;
using namespace forge::render;

namespace {

struct Timing {
    std::vector<f64> ms;
    void print(const char* name) {
        std::sort(ms.begin(), ms.end());
        f64 sum = 0;
        for (f64 v : ms) sum += v;
        FORGE_INFO("%-14s avg %.3f ms, p99 %.3f ms, worst %.3f ms", name, sum / static_cast<f64>(ms.size()),
                   ms[ms.size() * 99 / 100], ms.back());
    }
};

u32 hash32(u32 a, u32 b) {
    u32 h = a * 374761393u + b * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

} // namespace

int main(int argc, char** argv) {
    u32 count = 1'000'000;
    u32 frames = 200;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--sprites") == 0) count = static_cast<u32>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--frames") == 0) frames = static_cast<u32>(std::atoi(argv[++i]));
    }
    jobs::init();
    {
        // A crowd spread over an area a bit bigger than the view: most are on screen.
        const f32 view_w = 480, view_h = 270;
        std::vector<f32> x(count), y(count);
        for (u32 i = 0; i < count; ++i) {
            x[i] = static_cast<f32>(hash32(i, 1) % 5400) / 10.0f - 30.0f;
            y[i] = static_cast<f32>(hash32(i, 2) % 3300) / 10.0f - 30.0f;
        }
        SpriteBatch batch;
        std::unique_ptr<Sprite[]> out(new Sprite[count]);
        std::unique_ptr<u32[]> order(new u32[count]);
        const ViewBox view{0, 0, view_w, view_h};
        Timing push, sorted, unsorted;
        for (u32 f = 0; f < frames; ++f) {
            const f32 drift = static_cast<f32>(f % 50) * 0.1f;
            u64 t0 = time_now_ns();
            batch.begin(0, 0, count);
            jobs::parallel_for(count, 8192, [&](u32 b, u32 e) {
                Sprite* s = batch.push(e - b);
                for (u32 i = b; i < e; ++i) {
                    Sprite& sp = s[i - b];
                    sp.x = x[i] + drift;
                    sp.y = y[i];
                    sp.frame = i & 15;
                    sp.order = draw_order(1, sp.y, view_h);
                }
            });
            u64 t1 = time_now_ns();
            const u32 n = batch.finish(view, true, out.get(), order.get(), count).draws;
            u64 t2 = time_now_ns();
            if (f == 0) {
                FORGE_INFO("%u sprites, %u on screen, %u threads", count, n, jobs::thread_count());
                for (u32 i = 1; i < n; ++i)
                    if (out[order[i - 1]].order > out[order[i]].order) {
                        FORGE_ERROR("not sorted at %u", i);
                        return 1;
                    }
            }
            t2 = time_now_ns();
            batch.finish(view, false, out.get(), order.get(), count);
            u64 t3 = time_now_ns();
            push.ms.push_back(ns_to_ms(t1 - t0));
            sorted.ms.push_back(ns_to_ms(t2 - t1));
            unsorted.ms.push_back(ns_to_ms(t3 - t2));
        }
        push.print("push");
        sorted.print("cull + sort");
        unsorted.print("copy only");
    }
    jobs::shutdown();
    return 0;
}
