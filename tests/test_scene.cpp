#include "forge/core/jobs.h"
#include "forge/core/time.h"
#include "forge/scene/scene.h"
#include "forge/world/generators.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

struct SceneTestHealth {
    forge::f32 hp = 100;
    forge::i32 team = 0;
};
FORGE_REFLECT_DECLARE(SceneTestHealth)
FORGE_REFLECT(SceneTestHealth, 1) {
    t.field("hp", &SceneTestHealth::hp);
    t.field("team", &SceneTestHealth::team);
}

using namespace forge;
using namespace forge::world;
using namespace forge::scene;

namespace {

struct PoolScope {
    PoolScope() { jobs::init(3); }
    ~PoolScope() { jobs::shutdown(); }
};

WorldDesc tight_desc() {
    WorldDesc d;
    d.load_margin = 0;
    d.keep_extra = 0;
    return d;
}

void settle(World& w, Scene& s, const Rect& view) {
    for (int i = 0; i < 4; ++i) {
        s.update();
        w.update(view);
        w.finish_loading();
    }
    s.update();
}

const Rect kHome{0, 0, 256, 256}; // chunks [0,4) × [0,4)
const Rect kAway{100'000, 100'000, 100'064, 100'064};

} // namespace

TEST_CASE("positions split into chunk and local tiles") {
    const Position p = Position::at_tile(-1.5, 130.25);
    CHECK(p.cx == -1);
    CHECK(p.x == doctest::Approx(62.5));
    CHECK(p.cy == 2);
    CHECK(p.y == doctest::Approx(2.25));
    CHECK(p.tile_x() == doctest::Approx(-1.5));
}

TEST_CASE("radius queries match a brute-force search") {
    PoolScope pool;
    World w(tight_desc(), std::make_shared<TopDownGenerator>(1));
    Scene s(w);
    settle(w, s, kHome);

    std::mt19937 rng(7);
    std::uniform_real_distribution<f64> coord(0.0, 256.0);
    std::vector<std::pair<flecs::entity_t, std::pair<f64, f64>>> all;
    for (int i = 0; i < 5000; ++i) {
        const f64 x = coord(rng), y = coord(rng);
        flecs::entity e = s.spawn(Position::at_tile(x, y));
        REQUIRE(e.is_valid());
        all.push_back({e.id(), {x, y}});
    }
    s.update();
    CHECK(s.stats().entities == 5000);

    for (int q = 0; q < 50; ++q) {
        const f64 qx = coord(rng), qy = coord(rng);
        const f32 r = 3.0f + static_cast<f32>(q % 20);
        std::vector<flecs::entity_t> got;
        s.query_radius(qx, qy, r, got);
        std::set<flecs::entity_t> expect;
        for (const auto& [e, p] : all) {
            const f64 dx = p.first - qx, dy = p.second - qy;
            if (dx * dx + dy * dy <= static_cast<f64>(r) * r - 1e-3) expect.insert(e);
        }
        std::set<flecs::entity_t> got_set(got.begin(), got.end());
        // Every entity clearly inside is found (floats may decide the rim either way).
        for (flecs::entity_t e : expect) CHECK(got_set.count(e) == 1);
        CHECK(got.size() == got_set.size());
    }
}

TEST_CASE("entities leave and return with their chunk") {
    PoolScope pool;
    World w(tight_desc(), std::make_shared<TopDownGenerator>(1));
    Scene s(w);
    s.register_component<SceneTestHealth>();
    settle(w, s, kHome);

    flecs::entity e = s.spawn(Position::at_tile(10.5, 20.25));
    e.set<SceneTestHealth>({42.0f, 3});
    s.spawn(Position::at_tile(70, 70));
    CHECK_FALSE(s.spawn(Position::at_tile(5000, 5000)).is_valid()); // not loaded
    s.update();
    CHECK(s.count_in_chunk({0, 0}) == 1);

    settle(w, s, kAway);
    CHECK(s.stats().entities == 0);
    CHECK(s.ecs().count<Position>() == 0);
    CHECK(s.stats().stored_chunks >= 2);

    settle(w, s, kHome);
    CHECK(s.stats().entities == 2);
    std::vector<flecs::entity_t> found;
    s.query_radius(10.5, 20.25, 0.5f, found);
    REQUIRE(found.size() == 1);
    flecs::entity back = s.ecs().entity(found[0]);
    const SceneTestHealth* h = back.try_get<SceneTestHealth>();
    REQUIRE(h != nullptr);
    CHECK(h->hp == 42.0f);
    CHECK(h->team == 3);
}

TEST_CASE("walking into an unloaded chunk packs the entity there") {
    PoolScope pool;
    World w(tight_desc(), std::make_shared<TopDownGenerator>(1));
    Scene s(w);
    settle(w, s, kHome);
    flecs::entity e = s.spawn(Position::at_tile(250, 10));
    s.update();
    // Moves 20 tiles right: into chunk (4, 0), which is not loaded.
    Position p = *e.try_get<Position>();
    p.x += 20;
    e.set<Position>(p);
    s.update();
    CHECK(s.stats().moved_out == 1);
    CHECK(s.ecs().count<Position>() == 0);

    settle(w, s, {256, 0, 320, 64});
    CHECK(s.count_in_chunk({4, 0}) == 1);
}

TEST_CASE("populate runs once per chunk") {
    PoolScope pool;
    World w(tight_desc(), std::make_shared<TopDownGenerator>(1));
    Scene s(w);
    int calls = 0;
    s.set_populator([&](ChunkCoord c, Scene& scene) {
        ++calls;
        scene.spawn(Position{c.x, c.y, 32, 32});
    });
    settle(w, s, kHome);
    CHECK(calls == 16);
    CHECK(s.stats().entities == 16);

    // Kill them all, leave and come back: nothing is repopulated.
    s.ecs().delete_with<Position>();
    s.update();
    settle(w, s, kAway);
    const int after_away = calls;
    settle(w, s, kHome);
    CHECK(calls == after_away);
    CHECK(s.stats().entities == 0);
}

TEST_CASE("entities are saved and come back in a new session") {
    PoolScope pool;
    const auto dir = std::filesystem::temp_directory_path() / ("forge_scene_" + std::to_string(time_now_ns()));
    auto gen = std::make_shared<TopDownGenerator>(1);
    {
        World w(tight_desc(), gen);
        Scene s(w);
        s.register_component<SceneTestHealth>();
        REQUIRE(w.open_save(dir));
        REQUIRE(s.open_save(dir));
        settle(w, s, kHome);
        s.spawn(Position::at_tile(1, 1)).set<SceneTestHealth>({7.0f, 1});
        settle(w, s, kAway); // packed into memory
        s.spawn(Position::at_tile(100'010, 100'010));
        s.update();
        const SceneSaveReport r = s.save();
        CHECK(r.ok);
        CHECK(s.stats().stored_chunks == 0);
    }
    {
        World w(tight_desc(), gen);
        Scene s(w);
        s.register_component<SceneTestHealth>();
        REQUIRE(w.open_save(dir));
        REQUIRE(s.open_save(dir));
        bool populated = false;
        s.set_populator([&](ChunkCoord, Scene&) { populated = true; });
        settle(w, s, kHome);
        CHECK_FALSE(populated); // the save remembers these chunks were visited
        std::vector<flecs::entity_t> found;
        s.query_radius(1, 1, 1, found);
        REQUIRE(found.size() == 1);
        CHECK(s.ecs().entity(found[0]).try_get<SceneTestHealth>()->hp == 7.0f);
        settle(w, s, kAway);
        CHECK(s.stats().entities == 1);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
