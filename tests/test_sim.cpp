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

constexpr TileId kRock = 1, kLedge = 2;

// A test room: floor from y = 10 down, a wall at x = 20 (y 0..9), a one-way
// ledge at y = 5 for x 30..35, open sky above.
class RoomGenerator final : public Generator {
public:
    void generate(ChunkCoord coord, const ChunkTiles& out) const override {
        for (u32 l = 0; l < out.layer_count; ++l) {
            TileId* tiles = out.layer(l);
            for (i32 ly = 0; ly < kChunkSize; ++ly)
                for (i32 lx = 0; lx < kChunkSize; ++lx) {
                    const i32 x = coord.x * kChunkSize + lx, y = coord.y * kChunkSize + ly;
                    TileId t = 0;
                    if (y >= 10) t = kRock;
                    else if (x == 20 && y >= 0) t = kRock;
                    else if (y == 5 && x >= 30 && x <= 35) t = kLedge;
                    tiles[ly * kChunkSize + lx] = l == 1 ? t : 0;
                }
        }
    }
};

struct Room {
    World world;
    scene::Scene scene;
    Simulation sim;
    Rect view{-64, -64, 128, 64};

    explicit Room(const SimDesc& desc = sim_desc(), i32 load_margin = 0)
        : world(world_desc(load_margin), std::make_shared<RoomGenerator>()), scene(world), sim(world, scene, desc) {
        sim.collision().set(kRock, TileShape::Solid);
        sim.collision().set(kLedge, TileShape::Platform);
        sim.update(0, view);
        world.finish_loading();
        sim.update(0, view);
    }

    static WorldDesc world_desc(i32 load_margin) {
        WorldDesc d;
        d.load_margin = load_margin;
        d.keep_extra = 0;
        return d;
    }
    static SimDesc sim_desc() {
        SimDesc d;
        d.gravity_y = 40;
        return d;
    }

    flecs::entity body_at(f64 x, f64 y, Body b = {}) {
        flecs::entity e = scene.spawn(Position::at_tile(x, y));
        e.set<Body>(b);
        return e;
    }
    void run(u32 ticks) {
        for (u32 i = 0; i < ticks; ++i) sim.update(1.0 / 60.0, view);
    }
};

} // namespace

TEST_CASE("fixed-step clock") {
    SimClock clock(60, 4);
    CHECK(clock.advance(1.0 / 60.0 + 1e-9) == 1);
    CHECK(clock.advance(0.004) == 0);
    CHECK(clock.alpha() > 0.2f);
    CHECK(clock.advance(0.013) == 1);
    // A long stall runs at most 4 ticks and drops the rest.
    CHECK(clock.advance(1.0) == 4);
    CHECK(clock.dropped_seconds() > 0.9);
    CHECK(clock.tick() == 6);
}

TEST_CASE("bodies fall onto the floor and stop at walls") {
    PoolScope pool;
    Room room;
    flecs::entity faller = room.body_at(5.5, 2);
    Body fast;
    fast.vx = 200; // faster than a tile per step: must not pass through the wall
    fast.gravity = 0;
    flecs::entity runner = room.body_at(15.5, 8, fast);
    room.run(90);

    const Position* p = faller.try_get<Position>();
    const Body* b = faller.try_get<Body>();
    CHECK(std::fabs(p->tile_y() + b->half_h - 10.0) < 1e-3);
    CHECK((b->contacts & OnGround) != 0);

    const Position* rp = runner.try_get<Position>();
    const Body* rb = runner.try_get<Body>();
    CHECK(std::fabs(rp->tile_x() + rb->half_w - 20.0) < 1e-3);
    CHECK(rb->vx == 0);
}

// Step 14.2c: the hero meets an enemy (bodies do not collide with each other, only with tiles): which face it came
// through, from where both were before the move.
TEST_CASE("touch_side tells top, bottom and side by where the boxes were before the move") {
    auto box = [](f64 x, f64 y, f64 hw, f64 hh) { return Box{x, y, hw, hh}; };
    // An enemy standing on a floor at y 10 (its top at 9.2), a hero 0.38 × 0.92 around it.
    const Box enemy = box(10, 9.6, 0.35, 0.4);
    // Falling onto it: its bottom over the enemy's top before, inside it now.
    CHECK(touch_side(box(10.2, 8.18, 0.38, 0.92), box(10.2, 8.48, 0.38, 0.92), enemy, enemy) == Touch::Top);
    // The same move that also met the floor (its speed is 0 after it): still the top, by where it was.
    CHECK(touch_side(box(10.2, 8.27, 0.38, 0.92), box(10.2, 9.08, 0.38, 0.92), enemy, enemy) == Touch::Top);
    // Walking into it along the floor: the side.
    CHECK(touch_side(box(9.2, 9.08, 0.38, 0.92), box(9.35, 9.08, 0.38, 0.92), enemy, enemy) == Touch::Side);
    CHECK(touch_side(box(10.8, 9.08, 0.38, 0.92), box(10.65, 9.08, 0.38, 0.92), enemy, enemy) == Touch::Side);
    // Not touching now: nothing, from anywhere.
    CHECK(touch_side(box(10.2, 7.0, 0.38, 0.92), box(10.2, 7.5, 0.38, 0.92), enemy, enemy) == Touch::None);
    CHECK(touch_side(box(9.0, 9.08, 0.38, 0.92), box(9.25, 9.08, 0.38, 0.92), enemy, enemy) == Touch::None);
    // An enemy falling onto the hero's head: the hero came through its bottom (the hero did not move).
    const Box hero = box(10, 9.08, 0.38, 0.92); // its top at 8.16
    CHECK(touch_side(hero, hero, box(10.1, 7.7, 0.35, 0.4), box(10.1, 7.9, 0.35, 0.4)) == Touch::Bottom);
    // Jumping up into a hovering one: the bottom too.
    CHECK(touch_side(box(10, 7.6, 0.38, 0.92), box(10, 7.4, 0.38, 0.92), box(10, 6.2, 0.35, 0.4), box(10, 6.2, 0.35, 0.4)) ==
          Touch::Bottom);
    // The enemy hops up into the hero's feet as the hero rises slower: against the enemy the hero came down, onto its top.
    CHECK(touch_side(box(10, 8.18, 0.38, 0.92), box(10, 8.0, 0.38, 0.92), box(10, 9.6, 0.35, 0.4), box(10, 9.0, 0.35, 0.4)) ==
          Touch::Top);
    // Rising beside it, its column reached in the same move: the side, not the bottom.
    CHECK(touch_side(box(9.2, 9.0, 0.38, 0.92), box(9.4, 8.8, 0.38, 0.92), enemy, enemy) == Touch::Side);
    // Already inside each other before the move (after a load, or touching at its side for a while): the side.
    CHECK(touch_side(box(10.2, 8.9, 0.38, 0.92), box(10.2, 9.0, 0.38, 0.92), enemy, enemy) == Touch::Side);
}

TEST_CASE("touch_side at a corner: the face whose gap closed last") {
    auto box = [](f64 x, f64 y, f64 hw, f64 hh) { return Box{x, y, hw, hh}; };
    const Box enemy = box(10, 9.6, 0.35, 0.4); // its box: x 9.65..10.35, y 9.2..10
    // Apart both ways before (0.1 over its top, 0.1 to its left); now inside both ways.
    // Down 0.5 and right 0.2: over its top in 0.2 of the move, beside it in 0.5: through the side.
    CHECK(touch_side(box(9.17, 8.18, 0.38, 0.92), box(9.37, 8.68, 0.38, 0.92), enemy, enemy) == Touch::Side);
    // Down 0.2 and right 0.5: beside it in 0.2, over its top in 0.5: through the top.
    CHECK(touch_side(box(9.17, 8.18, 0.38, 0.92), box(9.67, 8.38, 0.38, 0.92), enemy, enemy) == Touch::Top);
    // Already over its column before (apart sideways ≤ 0): only the vertical gap counts.
    CHECK(touch_side(box(9.5, 8.18, 0.38, 0.92), box(9.9, 8.5, 0.38, 0.92), enemy, enemy) == Touch::Top);
}

TEST_CASE("touch_side: rounding over a top face, eps") {
    auto box = [](f64 x, f64 y, f64 hw, f64 hh) { return Box{x, y, hw, hh}; };
    const Box enemy = box(10, 9.6, 0.35, 0.4);
    // A hair (0.03) inside before, falling: still the top; 0.2 inside: the side.
    CHECK(touch_side(box(10, 8.31, 0.38, 0.92), box(10, 8.6, 0.38, 0.92), enemy, enemy) == Touch::Top);
    CHECK(touch_side(box(10, 8.48, 0.38, 0.92), box(10, 8.7, 0.38, 0.92), enemy, enemy) == Touch::Side);
    CHECK(touch_side(box(10, 8.48, 0.38, 0.92), box(10, 8.7, 0.38, 0.92), enemy, enemy, 0.25) == Touch::Top);
}

// The hero's move, as move_body makes it: crossing the enemy's top and meeting the floor in one tick zeroes its speed;
// where it was before the move still says it came from above.
TEST_CASE("a body that met the floor in the move that took it into another came through its top") {
    PoolScope pool;
    Room room;
    Body enemy_body;
    enemy_body.half_w = 0.35f;
    enemy_body.half_h = 0.4f;
    flecs::entity enemy = room.body_at(10.5, 9.6, enemy_body);
    room.run(5);
    Body hero_body;
    hero_body.half_w = 0.38f;
    hero_body.half_h = 0.92f;
    hero_body.vy = 49; // a fall at nearly the most: 0.82 of a tile a tick, more than the enemy is tall
    flecs::entity hero = room.body_at(10.6, 9.2 - 0.92 - 0.01, hero_body);
    for (u32 i = 0; i < 3 && hero.get<Body>().last_dy == 0; ++i) room.run(1); // its first tick
    const Body& hb = hero.get<Body>();
    const Body& eb = enemy.get<Body>();
    const Position& hp = hero.get<Position>();
    const Position& ep = enemy.get<Position>();
    CHECK(hb.vy == 0);
    CHECK((hb.contacts & OnGround) != 0);
    CHECK(std::fabs(hp.tile_y() + 0.92 - 10.0) < 1e-3);
    const Box before{hp.tile_x() - hb.last_dx, hp.tile_y() - hb.last_dy, hb.half_w, hb.half_h};
    const Box now{hp.tile_x(), hp.tile_y(), hb.half_w, hb.half_h};
    const Box e_before{ep.tile_x() - eb.last_dx, ep.tile_y() - eb.last_dy, eb.half_w, eb.half_h};
    const Box e_now{ep.tile_x(), ep.tile_y(), eb.half_w, eb.half_h};
    CHECK(hb.last_dy > 0.5f);
    CHECK(touch_side(before, now, e_before, e_now) == Touch::Top);
}

TEST_CASE("one-way ledges hold from above and let through from below") {
    PoolScope pool;
    Room room;
    flecs::entity lands = room.body_at(32.5, 2);
    Body jumper;
    jumper.vy = -40;
    flecs::entity passes = room.body_at(33.5, 8.5, jumper);
    room.run(8);
    // The jumper is above the ledge's row already, moving up.
    CHECK(passes.try_get<Position>()->tile_y() < 5.0);
    room.run(120);
    CHECK(std::fabs(lands.try_get<Position>()->tile_y() + 0.4 - 5.0) < 1e-3);
    // It falls back down and lands on the ledge it jumped through.
    CHECK(std::fabs(passes.try_get<Position>()->tile_y() + 0.4 - 5.0) < 1e-3);
}

TEST_CASE("contact and trigger events") {
    PoolScope pool;
    Room room;
    flecs::entity door = room.scene.spawn(Position::at_tile(10.5, 9));
    door.set<Trigger>({1.5f});
    Body walker;
    walker.vx = 6;
    flecs::entity e = room.body_at(4.5, 8.5, walker);
    room.run(1);
    room.run(1); // the scene index sees the walker from here on

    bool landed = false, entered = false, left = false;
    for (u32 i = 0; i < 180; ++i) {
        // Keep walking (hitting the ground does not stop it).
        e.get_mut<Body>().vx = 6;
        room.run(1);
        for (const ContactEvent& c : room.sim.frame_events().contacts)
            if (c.entity == e.id() && (c.began & OnGround)) landed = true;
        for (const TriggerEvent& t : room.sim.frame_events().triggers) {
            CHECK(t.trigger == door.id());
            CHECK(t.other == e.id());
            if (t.entered) entered = true;
            else if (entered) left = true;
        }
    }
    CHECK(landed);
    CHECK(entered);
    CHECK(left);
}

TEST_CASE("zones: active near the focus, near further out, asleep beyond") {
    PoolScope pool;
    WorldDesc wd;
    wd.load_margin = 4;
    wd.keep_extra = 0;
    World world(wd, std::make_shared<RoomGenerator>());
    const Rect focus{0, 0, 64, 64}; // chunk (0, 0)
    world.update(focus);
    world.finish_loading();
    world.update(focus);
    ZoneDesc zd;
    zd.active_margin = 1;
    zd.near_margin = 3;
    zd.near_every = 4;
    Zones zones(zd);
    zones.update(world, std::span<const Rect>(&focus, 1), 0);
    CHECK(zones.zone_of({0, 0}) == Zone::Active);
    CHECK(zones.zone_of({1, -1}) == Zone::Active);
    CHECK(zones.zone_of({3, 0}) == Zone::Near);
    CHECK(zones.zone_of({4, 0}) == Zone::Asleep);

    // A Near chunk runs once every 4 ticks, with 4 ticks' worth of time.
    u32 total = 0, runs = 0;
    for (u64 t = 0; t < 40; ++t) {
        const u32 due = zones.ticks_due({3, 0}, t);
        total += due;
        runs += due > 0;
    }
    CHECK(runs == 10);
    CHECK(total == 40);

    // The focus moves away and comes back: the chunk reports how long it slept.
    const Rect away{640, 0, 704, 64};
    world.update(std::span<const Rect>(&away, 1));
    zones.update(world, std::span<const Rect>(&away, 1), 100);
    CHECK(zones.zone_of({0, 0}) == Zone::Asleep);
    world.update(focus);
    world.finish_loading();
    world.update(focus);
    zones.update(world, std::span<const Rect>(&focus, 1), 400);
    bool found = false;
    for (const ChunkWake& w : zones.woken())
        if (w.coord == ChunkCoord{0, 0}) {
            found = true;
            CHECK(w.slept_ticks == 400);
        }
    CHECK(found);
}

TEST_CASE("systems see only entities whose chunk is due") {
    PoolScope pool;
    SimDesc desc = Room::sim_desc();
    desc.zones.active_margin = 0;
    desc.zones.near_margin = 0;
    Room room(desc);
    struct Counter {
        f32 time = 0;
    };
    flecs::entity inside = room.scene.spawn(Position::at_tile(5, 5));
    inside.set<Counter>({});
    // Loaded (the view reaches it) but outside the focus margin: asleep.
    const Rect small{0, 0, 64, 64};
    room.view = small;
    auto q = room.scene.ecs().query<Position, Counter>();
    room.sim.add_system([&](const TickContext& ctx) {
        each_due(ctx, q, [](flecs::entity_t, f32 dt, Position&, Counter& c) { c.time += dt; });
    });
    room.run(60);
    CHECK(std::fabs(inside.try_get<Counter>()->time - 1.0f) < 1e-3f);
}

TEST_CASE("gravity: world pull plus local sources") {
    GravityField field;
    field.set_world(0, 40);
    f32 gx = 0, gy = 0;
    field.at(5, 5, gx, gy);
    CHECK(gx == 0);
    CHECK(gy == 40);

    GravitySource add;
    add.radius = 10;
    add.strength = 20;
    GravitySource replace = add;
    replace.replace = true;
    GravitySource fading = add;
    fading.fade = true;
    GravitySource wind;
    wind.toward_center = false;
    wind.dir_x = -2; // only the direction counts
    wind.dir_y = 0;
    wind.radius = 4;
    wind.strength = 10;

    const GravityField::Placed a[] = {{100, 100, add}, {1000, 100, replace}, {2000, 100, fading}, {3000, 100, wind}};
    field.build(a);
    field.at(105, 100, gx, gy); // pulled back toward x = 100, plus the world
    CHECK(gx == doctest::Approx(-20));
    CHECK(gy == doctest::Approx(40));
    field.at(1005, 100, gx, gy); // the world's pull is off inside
    CHECK(gx == doctest::Approx(-20));
    CHECK(gy == doctest::Approx(0));
    field.at(2005, 100, gx, gy); // halfway out: half strength
    CHECK(gx == doctest::Approx(-10));
    field.at(3001, 101, gx, gy);
    CHECK(gx == doctest::Approx(-10));
    CHECK(gy == doctest::Approx(40));
    field.at(120, 100, gx, gy); // out of reach
    CHECK(gx == 0);
    CHECK(gy == 40);
}

TEST_CASE("sideways gravity: the wall becomes the ground") {
    PoolScope pool;
    SimDesc desc = Room::sim_desc();
    desc.gravity_x = 40;
    desc.gravity_y = 0;
    Room room(desc);
    flecs::entity e = room.body_at(14.5, 5);
    room.run(90);
    const Body* b = e.try_get<Body>();
    CHECK(std::fabs(e.try_get<Position>()->tile_x() + b->half_w - 20.0) < 1e-3);
    CHECK((b->contacts & OnGround) != 0);
    CHECK((b->contacts & HitRight) != 0);

    // Turned over during play: now it falls up into the sky... and an
    // unloaded chunk above stops it like a ceiling.
    room.sim.set_gravity(0, -40);
    room.run(120);
    CHECK(e.try_get<Position>()->tile_y() < 5.0);
}

TEST_CASE("a local source pulls bodies into it") {
    PoolScope pool;
    Room room;
    flecs::entity well = room.scene.spawn(Position::at_tile(45.5, 3.5));
    GravitySource g;
    g.radius = 6;
    g.strength = 80;
    g.replace = true;
    well.set<GravitySource>(g);
    Body b;
    b.gravity = 1;
    flecs::entity e = room.body_at(41.5, 3.5, b);
    for (u32 i = 0; i < 60; ++i) {
        room.run(1);
        Body& body = e.get_mut<Body>();
        body.vx *= 0.8f; // damped so it settles in the middle
        body.vy *= 0.8f;
    }
    const Position* p = e.try_get<Position>();
    CHECK(std::fabs(p->tile_x() - 45.5) < 0.5);
    CHECK(std::fabs(p->tile_y() - 3.5) < 0.5);
}

TEST_CASE("rigid bodies fall, stack and report contacts") {
    PoolScope pool;
    Room room;
    // A tower of three crates dropped onto the floor (y = 10).
    std::vector<flecs::entity> crates;
    for (int i = 0; i < 3; ++i) {
        flecs::entity e = room.scene.spawn(Position::at_tile(6.5, 2.0 + i * 2.5));
        e.set<RigidBody>({});
        crates.push_back(e);
    }
    flecs::entity ball = room.scene.spawn(Position::at_tile(12.5, 1));
    RigidBody rb;
    rb.shape = static_cast<u8>(RigidShape::Circle);
    rb.half_w = 0.4f;
    ball.set<RigidBody>(rb);

    bool hit_floor = false;
    for (int i = 0; i < 240; ++i) {
        room.run(1);
        for (const RigidContact& c : room.sim.frame_events().rigid)
            if (c.b == 0 && (c.a == crates[2].id() || c.a == ball.id())) hit_floor = true;
    }
    CHECK(hit_floor);
    CHECK(room.sim.stats().rigid.live == 4);
    // Bottom crate on the floor, the others on top of it.
    for (int i = 0; i < 3; ++i) {
        const f64 y = crates[static_cast<usize>(2 - i)].try_get<Position>()->tile_y();
        CHECK(std::fabs(y - (9.5 - i)) < 0.1);
        CHECK(std::fabs(crates[static_cast<usize>(i)].try_get<Position>()->tile_x() - 6.5) < 0.2);
    }
    CHECK(std::fabs(ball.try_get<Position>()->tile_y() - 9.6) < 0.05);
}

TEST_CASE("rigid bodies wait while their chunk is not active") {
    PoolScope pool;
    SimDesc desc = Room::sim_desc();
    desc.zones.active_margin = 0;
    desc.zones.near_margin = 1;
    Room room(desc, 1); // chunks next to the view stay loaded
    room.view = {0, 0, 64, 64}; // chunk (0, 0) only is Active
    flecs::entity away = room.scene.spawn(Position::at_tile(70.5, 2)); // chunk (1, 0): Near
    RigidBody rb;
    rb.vx = 1;
    away.set<RigidBody>(rb);
    room.run(30);
    CHECK(away.try_get<Position>()->tile_y() == doctest::Approx(2.0));
    CHECK(room.sim.stats().rigid.live == 0);
    // The camera comes: it falls, keeping its saved sideways speed.
    room.view = {64, 0, 128, 64};
    room.run(60);
    CHECK(away.try_get<Position>()->tile_y() > 9.0);
    CHECK(away.try_get<Position>()->tile_x() > 70.6);
}
