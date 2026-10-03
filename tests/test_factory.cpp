#include "forge/core/jobs.h"
#include "forge/sim/factory.h"
#include "forge/sim/factory_sample.h"

#include <doctest/doctest.h>

#include <vector>

using namespace forge;
using namespace forge::sim;

namespace {

struct PoolScope {
    PoolScope() { jobs::init(3); }
    ~PoolScope() { jobs::shutdown(); }
};

constexpr f32 kDt = 1.0f / 60.0f;
constexpr ItemId kOre = 1, kPlate = 2, kCoal = 3;
constexpr FluidId kWater = 1, kSteam = 2;

void run(Factory& f, u32 ticks) {
    for (u32 i = 0; i < ticks; ++i) f.tick(kDt);
}

void belt_row(Factory& f, i32 x0, i32 x1, i32 y, Dir dir = Dir::East) {
    for (i32 x = x0; x <= x1; ++x) f.place_belt(x, y, dir);
}

u32 items_in(const Factory& f, world::Rect r) {
    u32 n = 0;
    f.for_each_item(r, [&](f32, f32, ItemId) { ++n; });
    return n;
}

MachineDesc sink_at(i32 x, i32 y) {
    MachineDesc d;
    d.kind = MachineKind::Sink;
    d.x = x;
    d.y = y;
    return d;
}

MachineDesc solar_at(i32 x, i32 y, f32 kw) {
    MachineDesc d;
    d.kind = MachineKind::Generator;
    d.x = x;
    d.y = y;
    d.power = kw;
    return d;
}

// Drill -> belt -> smelter (2×2, needs power) -> belt -> sink, with a pole
// and a solar panel next to the smelter.
struct Chain {
    MachineId drill, smelter, sink, solar = kNoMachine;
    explicit Chain(Factory& f, f32 solar_kw = 100, bool pole = true) {
        MachineDesc d;
        d.recipe.out = {kOre, 1};
        d.recipe.seconds = 0.5f;
        drill = f.place_machine(d);
        belt_row(f, 1, 5, 0);
        MachineDesc s;
        s.x = 6;
        s.w = s.h = 2;
        s.recipe.in[0] = {kOre, 1};
        s.recipe.out = {kPlate, 1};
        s.recipe.seconds = 1;
        s.power = 90;
        smelter = f.place_machine(s);
        belt_row(f, 8, 12, 0);
        sink = f.place_machine(sink_at(13, 0));
        if (pole) f.place_pole(7, 2);
        if (solar_kw > 0) solar = f.place_machine(solar_at(9, 3, solar_kw));
    }
};

} // namespace

TEST_CASE("belts carry items into a machine at their end") {
    PoolScope pool;
    Factory f;
    belt_row(f, 0, 9, 0);
    const MachineId sink = f.place_machine(sink_at(10, 0));
    run(f, 1);
    REQUIRE(f.insert_item(0, 0, kOre));
    CHECK(items_in(f, {0, 0, 10, 1}) == 1);
    // 9.5 tiles at 1.875 tiles a second: about 304 ticks.
    run(f, 290);
    CHECK(f.machine(sink).made == 0);
    run(f, 30);
    CHECK(f.machine(sink).made == 1);
    CHECK(f.stats().items == 0);
}

TEST_CASE("items pack up at a dead end, four per tile") {
    PoolScope pool;
    Factory f;
    belt_row(f, 0, 3, 0);
    run(f, 1);
    u32 inserted = 0;
    for (u32 i = 0; i < 2000; ++i) {
        if (f.insert_item(0, 0, kOre)) ++inserted;
        f.tick(kDt);
    }
    CHECK(inserted >= 15);
    CHECK(f.stats().items == inserted);
    CHECK(f.stats().items <= 16);
    // Packed against the end: the front item sits at the front edge.
    f32 front = 0;
    f.for_each_item({0, 0, 4, 1}, [&](f32 x, f32, ItemId) { front = std::max(front, x); });
    CHECK(front == doctest::Approx(4.0f));
}

TEST_CASE("a powered smelter turns ore into plates") {
    PoolScope pool;
    Factory f;
    Chain c(f);
    run(f, 60 * 30);
    const MachineState s = f.machine(c.smelter);
    CHECK(s.satisfaction == doctest::Approx(1.0f));
    CHECK(f.machine(c.drill).made > 20);
    // About one plate a second once ore arrives (the belts take ~6 s).
    CHECK(f.machine(c.sink).made > 15);
    CHECK(f.stats().power_nets == 1);
}

TEST_CASE("without power the smelter waits, with too little it is slower") {
    PoolScope pool;
    {
        Factory f;
        Chain c(f, 100, false); // no pole: not connected
        run(f, 60 * 20);
        CHECK(f.machine(c.smelter).made == 0);
        // The belt in front of it fills up, the drill stops when its belt is full.
        CHECK(f.stats().items > 15);
    }
    {
        Factory f;
        Chain c(f, 45); // half of what it needs
        run(f, 60 * 40);
        CHECK(f.machine(c.smelter).satisfaction == doctest::Approx(0.5f));
        const u32 made = f.machine(c.smelter).made;
        CHECK(made > 12);
        CHECK(made < 22);
    }
}

TEST_CASE("accumulators store spare power and give it back") {
    PoolScope pool;
    Factory f;
    Chain c(f, 0);
    MachineDesc a;
    a.kind = MachineKind::Accumulator;
    a.x = 9;
    a.y = 3;
    a.power = 300;
    a.capacity = 5000;
    const MachineId acc = f.place_machine(a);
    MachineDesc g = solar_at(8, 3, 200);
    const MachineId solar = f.place_machine(g);
    run(f, 60 * 10);
    CHECK(f.machine(acc).stored > 500);
    // Night: the panel goes; the accumulator keeps the smelter going.
    f.remove_machine(solar);
    const f32 stored = f.machine(acc).stored;
    const u32 made = f.machine(c.smelter).made;
    run(f, 60 * 5);
    CHECK(f.machine(acc).stored < stored);
    CHECK(f.machine(c.smelter).made > made);
}

TEST_CASE("pump, boiler and steam engine: fluids through pipes make power") {
    PoolScope pool;
    Factory f;
    // Pump at (0,0) -> water pipe (1..3,0) -> boiler (4,0) burning coal from
    // a belt -> steam pipe (5..7,0) -> engine (8,0) -> pole -> a consumer.
    MachineDesc pump;
    pump.recipe.fluid_out = kWater;
    pump.recipe.fluid_out_amount = 20;
    pump.recipe.seconds = 0.1f;
    f.place_machine(pump);
    for (i32 x = 1; x <= 3; ++x) f.place_pipe(x, 0);
    MachineDesc boiler;
    boiler.x = 4;
    boiler.recipe.in[0] = {kCoal, 1};
    boiler.recipe.fluid_in = kWater;
    boiler.recipe.fluid_in_amount = 60;
    boiler.recipe.fluid_out = kSteam;
    boiler.recipe.fluid_out_amount = 60;
    boiler.recipe.seconds = 1;
    const MachineId b = f.place_machine(boiler);
    for (i32 x = 5; x <= 7; ++x) f.place_pipe(x, 0);
    MachineDesc engine;
    engine.kind = MachineKind::Generator;
    engine.x = 8;
    engine.power = 600;
    engine.fuel = kSteam;
    engine.fuel_energy = 10; // 60 steam a second at full power
    const MachineId e = f.place_machine(engine);
    f.place_pole(9, 1);
    MachineDesc lamp;
    lamp.x = 10;
    lamp.recipe.seconds = 0.5f;
    lamp.power = 300;
    const MachineId user = f.place_machine(lamp);
    // Coal comes down a belt from above into the boiler.
    for (i32 y = -6; y <= -1; ++y) f.place_belt(4, y, Dir::South);
    run(f, 1);
    for (u32 i = 0; i < 60 * 20; ++i) {
        if (i % 30 == 0) f.insert_item(4, -6, kCoal);
        f.tick(kDt);
    }
    FluidId fluid = 0;
    CHECK(f.pipe_at(2, 0, &fluid));
    CHECK(fluid == kWater);
    CHECK(f.pipe_at(6, 0, &fluid));
    CHECK(fluid == kSteam);
    CHECK(f.machine(b).made > 5);
    CHECK(f.machine(e).output > 0);
    CHECK(f.machine(user).made > 10);
}

TEST_CASE("rebuilding belts keeps the items on them") {
    PoolScope pool;
    Factory f;
    belt_row(f, 0, 7, 0);
    run(f, 1);
    for (i32 x = 0; x < 8; ++x) REQUIRE(f.insert_item(x, 0, kOre));
    // Extend the line, and turn it at the end: items stay where they were.
    f.place_belt(8, 0, Dir::South);
    f.place_belt(8, 1, Dir::South);
    run(f, 1);
    CHECK(f.stats().items == 8);
    CHECK(f.stats().lines == 1);
    // Cut it in the middle: the item on that tile is lost, the rest stay.
    f.remove_belt(4, 0);
    run(f, 1);
    CHECK(f.stats().items == 7);
    CHECK(f.stats().lines == 2);
    // Items run to the gap and wait there; the far part runs out to its end.
    run(f, 600);
    CHECK(items_in(f, {0, 0, 4, 1}) == 4);
    CHECK(items_in(f, {5, 0, 9, 2}) == 3);
}

TEST_CASE("two belts merge into one and both get through") {
    PoolScope pool;
    Factory f;
    // A from the west, B from the north, both into (5,0), on to a sink.
    belt_row(f, 0, 4, 0);
    for (i32 y = -5; y <= -1; ++y) f.place_belt(5, y, Dir::South);
    belt_row(f, 5, 9, 0);
    const MachineId sink = f.place_machine(sink_at(10, 0));
    run(f, 1);
    u32 put = 0;
    for (u32 i = 0; i < 60 * 30; ++i) {
        if (i % 20 == 0) {
            put += f.insert_item(0, 0, kOre);
            put += f.insert_item(5, -5, kPlate);
        }
        f.tick(kDt);
    }
    run(f, 60 * 10);
    CHECK(put > 100);
    CHECK(f.machine(sink).made == put);
}

TEST_CASE("a belt loop keeps its items going round") {
    PoolScope pool;
    Factory f;
    for (i32 i = 0; i < 4; ++i) {
        f.place_belt(i, 0, Dir::East);
        f.place_belt(4, i, Dir::South);
        f.place_belt(4 - i, 4, Dir::West);
        f.place_belt(0, 4 - i, Dir::North);
    }
    run(f, 1);
    CHECK(f.stats().lines == 1);
    for (i32 x = 0; x < 4; ++x) REQUIRE(f.insert_item(x, 0, kOre));
    run(f, 60 * 5); // over 9 tiles: everything is now on other sides
    CHECK(f.stats().items == 4);
    CHECK(items_in(f, {0, 0, 4, 1}) == 0);
}

TEST_CASE("factory state survives save and load") {
    PoolScope pool;
    Factory a;
    Chain c(a);
    run(a, 60 * 10);
    const std::vector<u8> bytes = a.save();
    Factory b;
    REQUIRE(b.load(bytes.data(), bytes.size()));
    b.tick(0); // apply (no time passes)
    a.tick(0);
    CHECK(b.stats().items == a.stats().items);
    CHECK(b.machine(c.sink).made == a.machine(c.sink).made);
    CHECK(b.machine(c.smelter).progress == doctest::Approx(a.machine(c.smelter).progress));
    run(a, 600);
    run(b, 600);
    CHECK(b.machine(c.sink).made == a.machine(c.sink).made);
    CHECK(b.stats().items == a.stats().items);
}

TEST_CASE("the sample block runs on its own steam power") {
    PoolScope pool;
    Factory f;
    const SampleBlock b = build_sample_block(f, 100, -50);
    run(f, 60 * 60);
    CHECK(f.stats().power_nets == 1);
    CHECK(f.machine(b.engine).output > 0);
    CHECK(f.machine(b.smelter).satisfaction == doctest::Approx(1.0f));
    // Two drills give 2 ore a second; plates come at 1.25 a second at most,
    // gears at half of that: about 0.6 a second once everything is running.
    CHECK(f.machine(b.sink).made > 20);
}
