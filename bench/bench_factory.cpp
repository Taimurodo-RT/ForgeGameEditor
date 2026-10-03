// Always-on factory benchmark: thousands of sample production blocks
// (drills, belts, smelters, assemblers, pipes, boilers, steam engines,
// power poles) spread over a 64k × 64k world, all running every tick.
//
//   forge_bench_factory [--blocks N] [--ticks N] [--threads N]

#include "forge/core/jobs.h"
#include "forge/core/log.h"
#include "forge/core/time.h"
#include "forge/sim/factory.h"
#include "forge/sim/factory_sample.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace forge;
using namespace forge::sim;

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

} // namespace

int main(int argc, char** argv) {
    u32 blocks = 10'000, ticks = 600, threads = 0;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--blocks") == 0) blocks = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--ticks") == 0) ticks = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--threads") == 0) threads = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
    }
    jobs::init(threads);
    {
        Factory f;
        // Blocks on a grid over the whole 64k world, with room between them.
        const u32 per_row = std::max(1u, static_cast<u32>(std::sqrt(static_cast<f64>(blocks))));
        const i32 step_x = std::min(65536 / static_cast<i32>(per_row), 4096);
        const i32 step_y = std::min(65536 / static_cast<i32>((blocks + per_row - 1) / per_row), 4096);
        u64 t0 = time_now_ns();
        std::vector<SampleBlock> list;
        list.reserve(blocks);
        for (u32 i = 0; i < blocks; ++i)
            list.push_back(build_sample_block(f, -32768 + static_cast<i32>(i % per_row) * std::max(step_x, kSampleBlockW + 4),
                                              -32768 + static_cast<i32>(i / per_row) * std::max(step_y, kSampleBlockH + 4)));
        const f64 place_ms = ns_to_ms(time_now_ns() - t0);
        t0 = time_now_ns();
        f.tick(0);
        const f64 first_ms = ns_to_ms(time_now_ns() - t0);
        const FactoryStats& s = f.stats();
        FORGE_INFO("%u blocks: %u machines, %u belt tiles in %u lines, %u power networks, %u pipe segments, %u threads",
                   blocks, s.machines, s.belt_tiles, s.lines, s.power_nets, s.pipe_segments, jobs::thread_count());
        FORGE_INFO("placing %.1f ms, first build %.1f ms", place_ms, first_ms);

        // Warm up: belts fill, boilers get water and coal.
        for (u32 i = 0; i < 60 * 20; ++i) f.tick(1.0f / 60.0f);
        Series belts, machines, power, fluids, total;
        for (u32 i = 0; i < ticks; ++i) {
            f.tick(1.0f / 60.0f);
            belts.v.push_back(s.belts_ms);
            machines.v.push_back(s.machines_ms);
            power.v.push_back(s.power_ms);
            fluids.v.push_back(s.fluids_ms);
            total.v.push_back(s.total_ms);
        }
        u64 gears = 0;
        for (const SampleBlock& b : list) gears += f.machine(b.sink).made;
        FORGE_INFO("running: %u items on belts, %llu gears made in total", s.items, static_cast<unsigned long long>(gears));
        belts.print("belts");
        machines.print("machines");
        power.print("power");
        fluids.print("fluids");
        total.print("total");

        // Building while it runs: a belt removed and put back in 100 blocks.
        Series rebuild;
        for (u32 i = 0; i < 100 && i < blocks; ++i) {
            const SampleBlock& b = list[i * (blocks / std::min(blocks, 100u))];
            const MachineDesc* d = f.machine_desc(b.ore_drills[0]);
            f.remove_belt(d->x + 5, d->y);
            f.tick(1.0f / 60.0f);
            rebuild.v.push_back(s.rebuild_ms);
            f.place_belt(d->x + 5, d->y, Dir::East);
            f.tick(1.0f / 60.0f);
            rebuild.v.push_back(s.rebuild_ms);
        }
        rebuild.print("edit");

        t0 = time_now_ns();
        const std::vector<u8> bytes = f.save();
        const f64 save_ms = ns_to_ms(time_now_ns() - t0);
        Factory g;
        t0 = time_now_ns();
        const bool ok = g.load(bytes.data(), bytes.size());
        const f64 load_ms = ns_to_ms(time_now_ns() - t0);
        g.tick(0);
        FORGE_INFO("save %.1f ms (%.1f MB), load %.1f ms, %s: %u items", save_ms, static_cast<f64>(bytes.size()) / 1e6,
                   load_ms, ok && g.stats().items == s.items ? "same" : "DIFFERENT", g.stats().items);
        if (!ok || g.stats().items != s.items) return 1;
    }
    jobs::shutdown();
    return 0;
}
