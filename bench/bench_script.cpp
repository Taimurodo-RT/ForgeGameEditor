// Script benchmark: many entities, each running a small Luau behaviour every
// tick (a patrol: read a variable, move, turn at the ends, count steps), the
// shape of logic a node graph compiles to. Then the same with half of them
// paused in forge.wait and with messages flying between them.
//
//   forge_bench_script [--entities N] [--frames N] [--interpret]
//
// --interpret turns off native code generation, to compare.

#include "forge/core/jobs.h"
#include "forge/core/log.h"
#include "forge/core/time.h"
#include "forge/script/host.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

using namespace forge;
using namespace forge::world;
using namespace forge::sim;
using namespace forge::script;

namespace {

class EmptyGenerator final : public Generator {
public:
    void generate(ChunkCoord, const ChunkTiles& out) const override {
        for (u32 l = 0; l < out.layer_count; ++l)
            for (i32 i = 0; i < kChunkTiles; ++i) out.layer(l)[i] = 0;
    }
};

struct Series {
    std::vector<f64> v;
    void print(const char* name) {
        std::sort(v.begin(), v.end());
        f64 sum = 0;
        for (f64 x : v) sum += x;
        FORGE_INFO("%-22s avg %7.3f ms   p99 %7.3f ms   worst %7.3f ms", name, sum / static_cast<f64>(v.size()),
                   v[v.size() * 99 / 100], v.back());
    }
};

const char* kPatrol = R"(
local S = {}
function S.on_start(self)
    forge.set_var(self, "dir", 1)
    forge.set_var(self, "steps", 0)
end
function S.on_tick(self, dt)
    local dir = forge.var(self, "dir", 1)
    local x, y = forge.entity.position(self)
    forge.entity.set_position(self, x + dir * 2 * dt, y)
    local steps = forge.var(self, "steps", 0) + 1
    if steps >= 120 then
        steps = 0
        forge.set_var(self, "dir", -dir)
    end
    forge.set_var(self, "steps", steps)
end
return S
)";

const char* kWaiter = R"(
local S = {}
function S.on_start(self)
    while true do
        forge.wait(0.5)
        forge.set_var(self, "beats", forge.var(self, "beats", 0) + 1)
    end
end
return S
)";

const char* kPinger = R"(
local S = {}
function S.on_tick(self, dt)
    local other = self + 1
    forge.send(other, "ping", 1)
end
function S.on_message(self, name, value, from)
    forge.set_var(self, "got", forge.var(self, "got", 0) + value)
end
return S
)";

void run(Simulation& sim, ScriptHost& scripts, const Rect& view, u32 frames, const char* name) {
    Series s;
    for (u32 f = 0; f < frames; ++f) {
        sim.update(1.0 / 60.0, view);
        s.v.push_back(scripts.stats().ms);
    }
    s.print(name);
    FORGE_INFO("  scripted %u, calls %u, waiting %u, messages %u, errors %u, Luau heap %.1f MB", scripts.stats().scripted,
               scripts.stats().calls, scripts.stats().waiting, scripts.stats().messages, scripts.stats().errors,
               static_cast<f64>(scripts.stats().memory) / 1e6);
}

} // namespace

int main(int argc, char** argv) {
    u32 entities = 10'000, frames = 300;
    bool native = true;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--entities") == 0 && i + 1 < argc) entities = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--interpret") == 0) native = false;
    }
    jobs::init();
    {
        World world(WorldDesc{}, std::make_shared<EmptyGenerator>());
        scene::Scene scene(world);
        Simulation sim(world, scene);
        ScriptDesc sd;
        sd.native = native;
        ScriptHost scripts(sim, scene, sd);
        const Rect view{0, 0, 1024, 512};
        for (u32 last = ~0u; world.stats().resident != last;) {
            last = world.stats().resident;
            sim.update(0, view);
            world.finish_loading();
        }
        if (!scripts.load("patrol", kPatrol) || !scripts.load("waiter", kWaiter) || !scripts.load("pinger", kPinger)) return 1;

        FORGE_INFO("%u entities with scripts, %s", entities, native ? "native code" : "interpreter");
        std::vector<flecs::entity> all;
        for (u32 i = 0; i < entities; ++i) {
            flecs::entity e = scene.spawn(scene::Position::at_tile(8 + (i % 1000) * 1.0, 8 + (i / 1000) * 4.0));
            e.set<Script>({"patrol"});
            all.push_back(e);
        }
        run(sim, scripts, view, frames, "patrol (on_tick)");
        if (std::getenv("FORGE_BENCH_EMPTY")) {
            scripts.load("empty", "return { on_tick = function(self, dt) end }");
            scripts.load("vars", "return { on_tick = function(self, dt) forge.set_var(self, 'a', forge.var(self, 'a', 0) + 1) end }");
            scripts.load("pos", "return { on_tick = function(self, dt) local x, y = forge.entity.position(self) forge.entity.set_position(self, x, y) end }");
            for (const char* m : {"empty", "vars", "pos"}) {
                for (flecs::entity e : all) e.set<Script>({m});
                run(sim, scripts, view, frames, m);
            }
        }

        for (u32 i = 0; i < entities; i += 2) all[i].set<Script>({"waiter"});
        run(sim, scripts, view, frames, "half waiting in loops");

        for (u32 i = 0; i < entities; ++i) all[i].set<Script>({i % 2 ? "patrol" : "pinger"});
        run(sim, scripts, view, frames, "messages");
    }
    jobs::shutdown();
    return 0;
}
