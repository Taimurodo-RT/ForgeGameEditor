#include "forge/core/jobs.h"
#include "forge/sim/simulation.h"

#include <doctest/doctest.h>

#include <cmath>
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

constexpr TileId kRock = 1;

// Flat floor from y = 10 down, open sky above.
class FloorGenerator final : public Generator {
public:
    void generate(ChunkCoord coord, const ChunkTiles& out) const override {
        for (u32 l = 0; l < out.layer_count; ++l)
            for (i32 ly = 0; ly < kChunkSize; ++ly)
                for (i32 lx = 0; lx < kChunkSize; ++lx)
                    out.layer(l)[ly * kChunkSize + lx] = (l == 1 && coord.y * kChunkSize + ly >= 10) ? kRock : 0;
    }
};

struct Room {
    World world;
    scene::Scene scene;
    Simulation sim;
    Rect view{-64, -64, 128, 64};

    Room() : world(world_desc(), std::make_shared<FloorGenerator>()), scene(world), sim(world, scene, sim_desc()) {
        sim.collision().set(kRock, TileShape::Solid);
        sim.update(0, view);
        world.finish_loading();
        sim.update(0, view);
    }
    static WorldDesc world_desc() {
        WorldDesc d;
        d.load_margin = 1;
        d.keep_extra = 0;
        return d;
    }
    static SimDesc sim_desc() {
        SimDesc d;
        d.gravity_y = 40;
        d.rewind.record = true;
        return d;
    }
    // Slides right at 4 tiles a second, no gravity.
    flecs::entity runner(f64 x, f64 y) {
        flecs::entity e = scene.spawn(Position::at_tile(x, y));
        Body b;
        b.vx = 4;
        b.gravity = 0;
        e.set<Body>(b);
        return e;
    }
    void run(u32 ticks) {
        for (u32 i = 0; i < ticks; ++i) sim.update(1.0 / 60.0, view);
    }
    static f64 x_of(flecs::entity e) { return e.try_get<Position>()->tile_x(); }
    static f64 y_of(flecs::entity e) { return e.try_get<Position>()->tile_y(); }
};

} // namespace

TEST_CASE("slow motion, stopped time and things outside time") {
    PoolScope pool;
    Room room;
    flecs::entity a = room.runner(0.5, 3);
    flecs::entity hero = room.runner(0.5, 6);
    hero.set<OutsideTime>({1.0f});

    room.sim.set_time_scale(0.25f);
    room.run(60);
    CHECK(Room::x_of(a) == doctest::Approx(1.5).epsilon(0.01)); // a quarter of 4 tiles
    CHECK(Room::x_of(hero) == doctest::Approx(4.5).epsilon(0.01));

    room.sim.set_time_scale(0);
    room.run(60);
    CHECK(Room::x_of(a) == doctest::Approx(1.5).epsilon(0.01)); // stopped
    CHECK(Room::x_of(hero) == doctest::Approx(8.5).epsilon(0.01));

    room.sim.set_time_scale(2);
    room.run(30);
    CHECK(Room::x_of(a) == doctest::Approx(5.5).epsilon(0.01)); // 4 tiles in half a second
}

TEST_CASE("time bubbles slow down or stop what is inside them") {
    PoolScope pool;
    Room room;
    flecs::entity frozen = room.runner(10.5, 3);
    flecs::entity slow = room.runner(40.5, 3);
    flecs::entity free = room.runner(70.5, 3);
    room.scene.spawn(Position::at_tile(11, 3)).set<TimeBubble>({4.0f, 0.0f, false});
    room.scene.spawn(Position::at_tile(41, 3)).set<TimeBubble>({6.0f, 0.5f, false});
    room.run(60);
    CHECK(Room::x_of(frozen) == doctest::Approx(10.5));
    CHECK(Room::x_of(slow) == doctest::Approx(42.5).epsilon(0.01));
    CHECK(Room::x_of(free) == doctest::Approx(74.5).epsilon(0.01));
}

TEST_CASE("rewind takes the world back: bodies, spawns, deaths and dug tiles") {
    PoolScope pool;
    Room room;
    flecs::entity faller = room.scene.spawn(Position::at_tile(5.5, 2));
    faller.set<Body>({});
    flecs::entity victim = room.runner(20.5, 3);
    flecs::entity hero = room.runner(30.5, 6);
    hero.set<OutsideTime>({1.0f});
    room.run(30);
    flecs::entity spawned = room.runner(50.5, 3);
    victim.destruct();
    room.world.set_tile(1, 5, 10, 0); // dig under the faller
    room.run(30);
    CHECK(Room::y_of(faller) > 9.0);
    const f64 hero_x = Room::x_of(hero);

    REQUIRE(room.sim.rewind().start(4)); // four times as fast as it went
    room.run(14);                        // 56 ticks back: near the start
    CHECK(Room::y_of(faller) < 2.5);
    CHECK(!spawned.is_alive());
    CHECK(victim.is_alive());
    CHECK(victim.try_get<Body>() != nullptr);
    CHECK(Room::x_of(victim) < 21.0);
    CHECK(room.world.tile(1, 5, 10) == kRock);
    // The hero kept going all along.
    CHECK(Room::x_of(hero) == doctest::Approx(hero_x + 14 * 4.0 / 60.0).epsilon(0.01));

    // Forward again from there: things fall and move as before.
    room.sim.rewind().stop();
    room.run(60);
    CHECK(Room::y_of(faller) > 8.0);
    CHECK(Room::x_of(victim) > 23.0);
    CHECK(room.sim.rewind().stats().frames > 20);
}

TEST_CASE("rewinding cannot go further back than it recorded") {
    PoolScope pool;
    Room room;
    flecs::entity a = room.runner(0.5, 3);
    CHECK(!room.sim.rewind().start());
    room.run(20);
    REQUIRE(room.sim.rewind().start(10));
    room.run(30);
    // Stuck at the oldest recording: a tick or so in.
    CHECK(Room::x_of(a) < 0.65);
    CHECK(room.sim.rewind().playing());
    room.sim.rewind().stop();
    CHECK(!room.sim.rewind().playing());
}

TEST_CASE("factories follow the world's time speed") {
    PoolScope pool;
    Room room;
    Factory& f = room.sim.factory();
    for (i32 x = 0; x < 10; ++x) f.place_belt(x, 0, Dir::East);
    MachineDesc sink;
    sink.kind = MachineKind::Sink;
    sink.x = 10;
    const MachineId s = f.place_machine(sink);
    room.run(1);
    REQUIRE(f.insert_item(0, 0, 1));
    room.sim.set_time_scale(0.5f);
    room.run(320); // the item needs ~304 ticks at full speed
    CHECK(f.machine(s).made == 0);
    room.run(300);
    CHECK(f.machine(s).made == 1);
}
