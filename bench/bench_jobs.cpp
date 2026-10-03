// Job system benchmark. Reports the frame-budget cost of the two shapes of
// work the engine produces every frame: many tiny independent jobs, and one
// big array split across all cores.

#include "forge/core/jobs.h"
#include "forge/core/log.h"
#include "forge/core/time.h"

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace forge;

namespace {

template <typename Fn>
f64 median_ms(int runs, Fn&& fn) {
    std::vector<f64> samples;
    for (int i = 0; i < runs; ++i) {
        const u64 t0 = time_now_ns();
        fn();
        samples.push_back(ns_to_ms(time_now_ns() - t0));
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

} // namespace

int main() {
    jobs::init();
    const u32 threads = jobs::thread_count();

    // 1. 10 000 empty jobs: pure scheduling overhead.
    constexpr u32 kJobs = 10'000;
    std::vector<Job> list(kJobs, Job{[](void*, u32) {}, nullptr, 0});
    const f64 empty_ms = median_ms(200, [&] {
        JobCounter c;
        jobs::submit(list.data(), kJobs, &c);
        jobs::wait(c);
    });

    // 2. 1 000 000 elements of light math, serial vs parallel.
    constexpr u32 kElems = 1'000'000;
    std::vector<f32> x(kElems, 1.0f), v(kElems, 0.5f);
    auto update = [&](u32 b, u32 e) {
        for (u32 i = b; i < e; ++i) {
            v[i] = v[i] * 0.999f - x[i] * 0.001f;
            x[i] += v[i] * 0.016f;
        }
    };
    const f64 serial_ms = median_ms(50, [&] { update(0, kElems); });
    const f64 parallel_ms = median_ms(50, [&] { jobs::parallel_for(kElems, 16'384, update); });

    std::printf("threads                      %u\n", threads);
    std::printf("10 000 empty jobs            %.3f ms  (%.0f ns per job)\n", empty_ms, empty_ms * 1e6 / kJobs);
    std::printf("1M elements, one thread      %.3f ms\n", serial_ms);
    std::printf("1M elements, parallel_for    %.3f ms  (x%.1f)\n", parallel_ms, serial_ms / parallel_ms);
    std::printf("budget at 60 FPS             16.667 ms\n");

    jobs::shutdown();
    return 0;
}
