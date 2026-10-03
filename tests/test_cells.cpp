#include "forge/core/jobs.h"
#include "forge/sim/simulation.h"

#include <doctest/doctest.h>

#include <cmath>
#include <functional>
#include <memory>

using namespace forge;
using namespace forge::world;
using namespace forge::sim;
using scene::Position;

namespace {

struct PoolScope {
    PoolScope() { jobs::init(3); }
    ~PoolScope() { jobs::shutdown(); }
};

constexpr TileId kRock = 1, kSand = 2, kObsidian = 3;

// Blocks from a function; walls and liquids empty.
class ShapeGenerator final : public Generator {
public:
    explicit ShapeGenerator(std::function<TileId(i32, i32)> fn) : fn_(std::move(fn)) {}
    void generate(ChunkCoord coord, const ChunkTiles& out) const override {
        for (u32 l = 0; l < out.layer_count; ++l) {
            TileId* tiles = out.layer(l);
            for (i32 ly = 0; ly < kChunkSize; ++ly)
                for (i32 lx = 0; lx < kChunkSize; ++lx)
                    tiles[ly * kChunkSize + lx] =
                        l == 1 ? fn_(coord.x * kChunkSize + lx, coord.y * kChunkSize + ly) : TileId{0};
        }
    }

private:
    std::function<TileId(i32, i32)> fn_;
};

struct Tank {
    World world;
    scene::Scene scene;
    Simulation sim;
    Rect view{0, 0, 128, 128};
    u8 water = 0, lava = 0;

    explicit Tank(std::function<TileId(i32, i32)> shape, bool level = true)
        : world(world_desc(), std::make_shared<ShapeGenerator>(std::move(shape))), scene(world), sim(world, scene, sim_desc()) {
        sim.collision().set(kRock, TileShape::Solid);
        sim.collision().set(kSand, TileShape::Solid);
        sim.collision().set(kObsidian, TileShape::Solid);
        LiquidKind w;
        w.level = level;
        water = sim.cells()->add_liquid(w);
        LiquidKind l;
        l.thickness = 4;
        lava = sim.cells()->add_liquid(l);
        sim.cells()->add_reaction(water, lava, kObsidian);
        sim.cells()->add_reaction(lava, water, kObsidian);
        sim.cells()->set_falling(kSand, true);
        for (u32 last = ~0u; world.stats().resident != last;) {
            last = world.stats().resident;
            sim.update(0, view);
            world.finish_loading();
        }
        sim.update(0, view);
    }
    static WorldDesc world_desc() {
        WorldDesc d;
        d.layer_count = 3;
        d.load_margin = 0;
        d.keep_extra = 0;
        return d;
    }
    static SimDesc sim_desc() {
        SimDesc d;
        d.gravity_y = 40;
        d.liquid_layer = 2;
        return d;
    }
    // Runs until nothing moves (or the limit); returns the ticks it took.
    u32 settle(u32 limit = 6000) {
        for (u32 t = 0; t < limit; ++t) {
            sim.update(1.0 / 60.0, view);
            if (t > 2 && sim.stats().cells.active_chunks == 0) return t;
        }
        return limit;
    }
    // Liquid in a column of cells, in full tiles.
    f64 column(i32 x, i32 y0, i32 y1) const {
        f64 sum = 0;
        for (i32 y = y0; y < y1; ++y) sum += liquid_amount(world.tile(2, x, y));
        return sum / kFull;
    }
};

// A U-tube: two shafts 2 wide joined by a channel at the bottom, rock around.
TileId u_tube(i32 x, i32 y) {
    const bool left = x >= 10 && x < 12 && y >= 2 && y < 40;
    const bool right = x >= 40 && x < 42 && y >= 2 && y < 40;
    const bool channel = x >= 10 && x < 42 && y >= 38 && y < 40;
    return left || right || channel ? TileId{0} : kRock;
}

void fill_left_shaft(Tank& tank, u32 tiles) {
    for (u32 i = 0; i < tiles; ++i) CHECK(tank.sim.cells()->pour(10 + static_cast<i32>(i % 2), 2 + static_cast<i32>(i / 2) % 30, tank.water, kFull));
}

} // namespace

TEST_CASE("liquid levels out in connected vessels") {
    PoolScope pool;
    Tank tank(u_tube, true);
    fill_left_shaft(tank, 60 + 40); // the channel holds 64, the rest rises in the shafts
    const u32 ticks = tank.settle();
    CHECK(ticks < 6000); // settles and sleeps
    const f64 left = tank.column(10, 2, 38) + tank.column(11, 2, 38);
    const f64 right = tank.column(40, 2, 38) + tank.column(41, 2, 38);
    MESSAGE("levelling: left shaft ", left / 2, " tiles, right shaft ", right / 2, " tiles, settled in ", ticks, " ticks");
    CHECK(left > 4);
    CHECK(std::fabs(left - right) < 3.0);
}

TEST_CASE("without levelling the far shaft stays dry") {
    PoolScope pool;
    Tank tank(u_tube, false);
    fill_left_shaft(tank, 100);
    CHECK(tank.settle() < 6000);
    const f64 left = tank.column(10, 2, 38) + tank.column(11, 2, 38);
    const f64 right = tank.column(40, 2, 38) + tank.column(41, 2, 38);
    MESSAGE("Terraria-like: left shaft ", left / 2, " tiles, right shaft ", right / 2, " tiles");
    CHECK(left > 10);
    CHECK(right < 1);
}

TEST_CASE("sand falls and piles into a slope") {
    PoolScope pool;
    Tank tank([](i32, i32 y) { return y >= 60 ? kRock : TileId{0}; });
    for (i32 y = 20; y < 50; ++y) tank.world.set_tile(1, 64, y, kSand);
    CHECK(tank.settle() < 6000);
    // Heights of the pile per column.
    i32 total = 0, tallest = 0, steepest = 0, prev = -1;
    for (i32 x = 30; x < 100; ++x) {
        i32 h = 0;
        for (i32 y = 0; y < 60; ++y) h += tank.world.tile(1, x, y) == kSand;
        total += h;
        tallest = std::max(tallest, h);
        if (prev >= 0) steepest = std::max(steepest, std::abs(h - prev));
        prev = h;
    }
    CHECK(total == 30); // nothing lost
    CHECK(tallest < 10);
    CHECK(steepest <= 1);
}

TEST_CASE("sand sinks through water; water meeting lava makes stone") {
    PoolScope pool;
    // A pit 10 wide, 10 deep.
    Tank tank([](i32 x, i32 y) { return (y >= 50 || ((x < 40 || x >= 50) && y >= 40)) ? kRock : TileId{0}; });
    for (i32 x = 40; x < 50; ++x)
        for (i32 y = 45; y < 50; ++y) tank.sim.cells()->pour(x, y, tank.water, kFull);
    tank.world.set_tile(1, 45, 30, kSand);
    tank.settle();
    CHECK(tank.world.tile(1, 45, 49) == kSand); // at the bottom, under the water
    CHECK(tank.column(45, 40, 50) > 3.5);

    // Lava poured onto the water: a crust of stone forms where they meet.
    for (i32 x = 42; x < 48; ++x) tank.sim.cells()->pour(x, 38, tank.lava, kFull);
    tank.settle();
    u32 stone = 0;
    for (i32 x = 40; x < 50; ++x)
        for (i32 y = 30; y < 50; ++y) stone += tank.world.tile(1, x, y) == kObsidian;
    CHECK(stone > 0);
}

TEST_CASE("bodies float in water") {
    PoolScope pool;
    Tank tank([](i32 x, i32 y) { return (y >= 50 || ((x < 40 || x >= 60) && y >= 30)) ? kRock : TileId{0}; });
    for (i32 x = 40; x < 60; ++x)
        for (i32 y = 32; y < 50; ++y) tank.sim.cells()->pour(x, y, tank.water, kFull);
    tank.settle();
    flecs::entity e = tank.scene.spawn(Position::at_tile(50, 45));
    Body b;
    b.gravity = 1;
    e.set<Body>(b);
    // Buoyancy 0.8: sinks slowly instead of dropping like a stone.
    for (u32 i = 0; i < 30; ++i) tank.sim.update(1.0 / 60.0, tank.view);
    CHECK(e.try_get<Body>()->liquid == tank.water);
    CHECK(e.try_get<Position>()->tile_y() < 47.0);
}
