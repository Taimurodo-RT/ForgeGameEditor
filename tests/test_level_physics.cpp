// The level's physics (the «Физика» mode): the world's pull in physics.json,
// gravity points as saved objects of the level, water and sand as its tiles,
// and the flow trial that runs them in the editor and puts everything back.
// Each check reads the level from its folder, as the game does.

#include "forge/core/file.h"
#include "forge/core/jobs.h"
#include "forge/data/json.h"
#include "forge/editor/document.h"
#include "forge/editor/undo.h"
#include "forge/level/level.h"
#include "forge/level/object_edit.h"
#include "forge/level/physics.h"
#include "forge/level/tile_edit.h"
#include "forge/sim/simulation.h"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace forge;
using namespace forge::world;
using namespace forge::level;
namespace fs = std::filesystem;

namespace {

struct PoolScope {
    PoolScope() { jobs::init(3); }
    ~PoolScope() { jobs::shutdown(); }
};

constexpr TileId kStone = 3, kSand = 4;
constexpr u32 kBlocks = 1, kLiquids = 2;
const TileId kWater = sim::make_liquid(1, sim::kFull);

// Air above y = 0, stone below; layers: walls, blocks, liquids.
struct FlatGenerator final : Generator {
    void generate(ChunkCoord coord, const ChunkTiles& out) const override {
        for (u32 l = 0; l < out.layer_count; ++l) {
            TileId* tiles = out.layer(l);
            for (i32 ly = 0; ly < kChunkSize; ++ly) {
                const i32 y = coord.y * kChunkSize + ly;
                for (i32 lx = 0; lx < kChunkSize; ++lx) tiles[ly * kChunkSize + lx] = l == kBlocks && y >= 0 ? kStone : 0;
            }
        }
    }
};

struct PhysModule final : LevelModule {
    std::vector<std::string> layers{"Стены", "Блоки", "Жидкости"};
    std::vector<TileDef> palette{{"stone", "Камень", "Земля", "", kBlocks, kStone, ""},
                                 {"sand", "Песок", "Земля", "", kBlocks, kSand, ""},
                                 {"water", "Вода", "Жидкости", "", kLiquids, kWater, ""}};
    std::string title() const override { return "Физика"; }
    WorldDesc world_desc() const override {
        WorldDesc d;
        d.layer_count = 3;
        d.load_margin = 0;
        d.keep_extra = 0;
        return d;
    }
    std::shared_ptr<const Generator> generator() const override { return std::make_shared<FlatGenerator>(); }
    void setup_scene(scene::Scene& scene) override { sim::register_components(scene); }
    const std::vector<std::string>& layer_names() const override { return layers; }
    const std::vector<TileDef>& tiles() const override { return palette; }
    void tile_icon(const TileDef&, u32 size, std::vector<u8>& rgba) const override { rgba.assign(size * size * 4, 255); }
    void start(f64& x, f64& y) const override { x = y = 0; }
    bool init_view(SDL_GPUDevice*, SDL_GPUTextureFormat) override { return true; }
    void shutdown_view() override {}
    void prepare_view(SDL_GPUCommandBuffer*, Level&, const render::Camera2D&, u32, u32, const ViewOptions&, f64) override {}
    void draw_view(SDL_GPUCommandBuffer*, SDL_GPURenderPass*) override {}
    LevelPhysics default_physics() const override { return {0, 40}; }
    std::vector<std::string> physics_fills() const override { return {"water", "sand"}; }
    std::unique_ptr<sim::CellSim> make_cells(sim::CollisionRules& rules) const override {
        rules.set(kStone, sim::TileShape::Solid);
        rules.set(kSand, sim::TileShape::Solid);
        auto cells = std::make_unique<sim::CellSim>(kBlocks, kLiquids);
        cells->add_liquid({});
        cells->set_falling(kSand, true);
        return cells;
    }
};

fs::path temp_folder(const char* name) {
    const fs::path p = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

std::string read_text(const fs::path& file) {
    std::vector<u8> bytes;
    if (!read_file(file, bytes)) return {};
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void write_text(const fs::path& file, const std::string& text) {
    REQUIRE(write_file_atomic(file, {reinterpret_cast<const u8*>(text.data()), text.size()}));
}

// The gravity points of a level now loaded: by LevelId.
struct Point {
    u64 id;
    f64 x, y;
    sim::GravitySource g;
};
std::vector<Point> points(Level& level) {
    std::vector<Point> out;
    level.scene().ecs().each([&](const LevelId& id, const scene::Position& p, const sim::GravitySource& g) {
        out.push_back({id.id, p.tile_x(), p.tile_y(), g});
    });
    return out;
}

// A point placed as the editor places it: made, then recorded.
u64 place_point(Level& level, editor::UndoStack& history, f64 x, f64 y, f32 radius, f32 strength) {
    flecs::entity e = level.scene().spawn(scene::Position::at_tile(x, y));
    REQUIRE(e.is_valid());
    sim::GravitySource g;
    g.radius = radius;
    g.strength = strength;
    e.set<sim::GravitySource>(g);
    ObjectSnapshot snap = snapshot(level, e);
    const u64 id = snap.id;
    history.execute(std::make_unique<ObjectsCommand>(level, std::vector<ObjectSnapshot>{std::move(snap)}, true, "Поставить"));
    history.seal();
    return id;
}

// Every tile of the ready chunks, to compare.
std::vector<TileId> all_tiles(World& world, const Rect& chunks) {
    std::vector<TileId> out;
    for (i32 cy = chunks.y0; cy < chunks.y1; ++cy)
        for (i32 cx = chunks.x0; cx < chunks.x1; ++cx) {
            const Chunk* c = world.find_chunk({cx, cy});
            REQUIRE(c);
            out.insert(out.end(), c->tiles, c->tiles + static_cast<usize>(world.layer_count()) * kChunkTiles);
        }
    return out;
}

// The game's way of reading a level: its world, its objects and its pull from the folder.
struct GameRun {
    World world;
    scene::Scene scene;
    std::unique_ptr<sim::Simulation> sim;
    LevelPhysics physics{0, 40};
    std::string physics_error;
    GameRun(const PhysModule& m, const fs::path& folder) : world(m.world_desc(), m.generator()), scene(world) {
        REQUIRE(world.open_save(folder));
        scene.register_component<LevelId>();
        REQUIRE(scene.open_save(folder));
        sim = std::make_unique<sim::Simulation>(world, scene, sim::SimDesc{});
        if (!load_physics(folder, physics, nullptr, &physics_error)) physics = {0, 40};
        sim->set_gravity(physics.gravity_x, physics.gravity_y);
    }
};

} // namespace

TEST_CASE("level physics: no physics.json is the game's pull; set, undone, saved and read back") {
    PoolScope pool;
    PhysModule module;
    const fs::path folder = temp_folder("forge_test_level_physics");
    editor::Document doc;
    editor::UndoStack history(doc);
    {
        Level level(module);
        REQUIRE(level.open(folder));
        CHECK(level.physics() == LevelPhysics{0, 40}); // an old level: the game's own
        CHECK(level.physics_error().empty());
        CHECK_FALSE(level.physics_changed());
        // Saving a level whose pull nobody touched writes no physics.json.
        CHECK(level.save().ok);
        CHECK_FALSE(fs::exists(folder / "physics.json"));

        history.execute(std::make_unique<SetPhysics>(level, level.physics(), LevelPhysics{-25, 0}, "сила"));
        history.execute(std::make_unique<SetPhysics>(level, level.physics(), LevelPhysics{-30, 0}, "сила"));
        CHECK(history.size() == 1); // a slider's drag: one entry
        CHECK(level.physics() == LevelPhysics{-30, 0});
        history.undo();
        CHECK(level.physics() == LevelPhysics{0, 40});
        CHECK_FALSE(level.physics_changed());
        history.redo();
        CHECK(level.physics_changed());
        const Level::SaveReport r = level.save();
        CHECK(r.ok);
        CHECK(r.physics);
        CHECK_FALSE(level.physics_changed());
    }
    {
        Level level(module);
        REQUIRE(level.open(folder));
        CHECK(level.physics() == LevelPhysics{-30, 0});
        CHECK(level.physics_error().empty());
        // Back to the game's own value: the file says so (it is not deleted).
        level.set_physics({0, 40});
        CHECK(level.save().ok);
    }
    LevelPhysics read{1, 2};
    bool found = false;
    CHECK(load_physics(folder, read, &found));
    CHECK(found);
    CHECK(read == LevelPhysics{0, 40});

    // The limits and no pull at all are valid values; one more is not.
    for (const LevelPhysics p : {LevelPhysics{0, 0}, LevelPhysics{200, -200}, LevelPhysics{-200, 200}, LevelPhysics{0.5f, 0}}) {
        CHECK(save_physics(folder, p));
        LevelPhysics back{7, 7};
        CHECK(load_physics(folder, back));
        CHECK(back == p);
    }
    CHECK_FALSE(valid_physics({200.5f, 0}));
    CHECK_FALSE(valid_physics({0, std::nanf("")}));
    CHECK_FALSE(valid_physics({INFINITY, 0}));
    std::string why;
    CHECK_FALSE(save_physics(folder, {0, std::nanf("")}, &why));
    CHECK(why.find("physics.json") != std::string::npos);
}

TEST_CASE("level physics: a bad physics.json opens the level with the game's pull and is not overwritten") {
    PoolScope pool;
    PhysModule module;
    const fs::path folder = temp_folder("forge_test_level_physics_bad");
    // A field missing keeps the game's value for it.
    write_text(folder / "physics.json", R"({"gravity_x": 5})");
    {
        Level level(module);
        REQUIRE(level.open(folder));
        CHECK(level.physics() == LevelPhysics{5, 40});
        CHECK(level.physics_error().empty());
    }
    const char* bad[] = {
        "это не JSON",
        R"({"gravity_y": "вниз"})",
        R"({"gravity_y": 1e9})",
        R"({"gravity_x": -201, "gravity_y": 0})",
        R"({"gravity_y": 1e999})",
        R"([0, 40])",
    };
    for (const char* text : bad) {
        CAPTURE(text);
        write_text(folder / "physics.json", text);
        Level level(module);
        REQUIRE(level.open(folder)); // the level itself opens
        CHECK(level.physics() == LevelPhysics{0, 40});
        CHECK(level.physics_error().find("physics.json") != std::string::npos);
        // Saved untouched: the author's file stays as it was.
        CHECK(level.save().ok);
        CHECK(read_text(folder / "physics.json") == text);
        // Set by the author: a good file again, and no error.
        level.set_physics({0, -40});
        CHECK(level.save().ok);
        CHECK(level.physics_error().empty());
        LevelPhysics back;
        CHECK(load_physics(folder, back));
        CHECK(back == LevelPhysics{0, -40});
    }
}

TEST_CASE("level physics: a write error is told and nothing is lost") {
    PoolScope pool;
    PhysModule module;
    const fs::path folder = temp_folder("forge_test_level_physics_write");
    Level level(module);
    REQUIRE(level.open(folder));
    // Something in the way of the file: a folder of that name.
    fs::create_directories(folder / "physics.json");
    level.ensure_loaded({0, -10, 4, -6});
    REQUIRE(level.set_tile(kBlocks, 1, -8, kSand));
    level.set_physics({40, 0});
    const Level::SaveReport r = level.save();
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("physics.json") != std::string::npos);
    CHECK(level.physics_changed()); // still to be saved
    CHECK(level.physics() == LevelPhysics{40, 0});
    CHECK(level.tile(kBlocks, 1, -8) == kSand);
    fs::remove_all(folder / "physics.json");
    CHECK(level.save().ok);
    Level again(module);
    REQUIRE(again.open(folder));
    CHECK(again.physics() == LevelPhysics{40, 0});
    again.ensure_loaded({0, -10, 4, -6});
    CHECK(again.tile(kBlocks, 1, -8) == kSand);
}

TEST_CASE("gravity points: saved with their ids, through unloading, a move over a chunk border and deletes") {
    PoolScope pool;
    PhysModule module;
    const fs::path folder = temp_folder("forge_test_level_points");
    editor::Document doc;
    editor::UndoStack history(doc);
    u64 a = 0, b = 0;
    {
        Level level(module);
        REQUIRE(level.open(folder));
        const Rect view{0, -64, 128, 0};
        level.update({&view, 1});
        level.ensure_loaded(view);
        a = place_point(level, history, 60.5, -20.5, 8, 40);
        b = place_point(level, history, 20.5, -30.5, 128, -200); // the largest, pushing
        CHECK(points(level).size() == 2);
        // Over the border of chunks x 0 | 1: one entry, the same id.
        history.execute(std::make_unique<MoveObjects>(level, std::vector<MoveObjects::Move>{{a, 60.5, -20.5, 70.5, -20.5}}, "Передвинуть"));
        history.seal();
        // Its radius, as the panel sets it.
        flecs::entity e = level.find(a);
        REQUIRE(e.is_valid());
        const reflect::TypeInfo* type = reflect::type_of<sim::GravitySource>();
        const std::string before = component_json(level, e, type);
        sim::GravitySource g = e.get<sim::GravitySource>();
        g.radius = 12.5f;
        g.fade = true;
        const std::string after = data::to_json(type, &g, false);
        history.execute(std::make_unique<SetObjectComponent>(level, a, 70.5, -20.5, type, before, after, "radius", "Радиус"));
        history.seal();
        // Far away and back: the chunks go and come again, the points with them.
        const Rect far{4096, -64, 4224, 0};
        for (int i = 0; i < 3; ++i) level.update({&far, 1});
        CHECK_FALSE(level.find(a).is_valid());
        for (int i = 0; i < 3; ++i) level.update({&view, 1});
        level.ensure_loaded(view);
        std::vector<Point> ps = points(level);
        REQUIRE(ps.size() == 2);
        for (const Point& p : ps) {
            if (p.id == a) {
                CHECK(p.x == doctest::Approx(70.5));
                CHECK(p.g.radius == 12.5f);
                CHECK(p.g.fade);
            } else {
                CHECK(p.id == b);
                CHECK(p.g.radius == 128);
                CHECK(p.g.strength == -200);
            }
        }
        REQUIRE(level.save().ok);
    }
    {
        // Opened again: the same two, deleted after loading, back with undo.
        Level level(module);
        REQUIRE(level.open(folder));
        history.clear();
        const Rect view{0, -64, 128, 0};
        level.ensure_loaded(view);
        REQUIRE(points(level).size() == 2);
        flecs::entity e = level.find(a);
        REQUIRE(e.is_valid());
        CHECK(e.get<scene::Position>().tile_x() == doctest::Approx(70.5));
        CHECK(e.get<sim::GravitySource>().radius == 12.5f);
        history.execute(std::make_unique<ObjectsCommand>(level, std::vector<ObjectSnapshot>{snapshot(level, e)}, false, "Удалить"));
        CHECK_FALSE(level.find(a).is_valid());
        CHECK(points(level).size() == 1);
        history.undo();
        REQUIRE(level.find(a).is_valid());
        CHECK(level.find(a).get<sim::GravitySource>().radius == 12.5f);
        CHECK(points(level).size() == 2);
        history.redo();
        CHECK(points(level).size() == 1);
        REQUIRE(level.save().ok);
    }
    {
        Level level(module);
        REQUIRE(level.open(folder));
        level.ensure_loaded({0, -64, 128, 0});
        const std::vector<Point> ps = points(level);
        REQUIRE(ps.size() == 1);
        CHECK(ps[0].id == b);
    }
}

TEST_CASE("gravity points in the game: centre, edge, outside, overlap, replace, zero and pushing, after a step") {
    PoolScope pool;
    PhysModule module;
    const fs::path folder = temp_folder("forge_test_level_pull");
    editor::Document doc;
    editor::UndoStack history(doc);
    {
        Level level(module);
        REQUIRE(level.open(folder));
        level.ensure_loaded({0, -64, 768, 0});
        sim::GravitySource g;
        auto put = [&](f64 x, f64 y, f32 r, f32 s, bool fade, bool replace) {
            flecs::entity e = level.scene().spawn(scene::Position::at_tile(x, y));
            REQUIRE(e.is_valid());
            g.radius = r;
            g.strength = s;
            g.fade = fade;
            g.replace = replace;
            e.set<sim::GravitySource>(g);
            level.id_of(e, true);
        };
        put(100.5, -20.5, 8, 40, false, false);  // plain
        put(200.5, -20.5, 8, 40, false, false);  // two that overlap
        put(206.5, -20.5, 12, 40, false, false);
        put(300.5, -20.5, 8, 40, true, false);   // fading
        put(400.5, -20.5, 8, 40, false, true);   // the world's pull off inside
        put(408.5, -20.5, 6, 20, false, false);  // and one more over it
        put(500.5, -20.5, 8, 0, false, false);   // zero
        put(520.5, -20.5, 8, 0, false, true);    // zero, but the world's pull off
        put(600.5, -20.5, 8, -40, false, false); // pushing away
        REQUIRE(level.save().ok);
    }
    GameRun game(module, folder);
    CHECK(game.physics_error.empty());
    const Rect view{0, -64, 768, 0};
    game.sim->update(0, view);
    game.world.finish_loading();
    game.sim->update(0, view);
    struct Probe {
        f64 x, y;
        f32 gx, gy;
        const char* what;
        flecs::entity e;
    };
    std::vector<Probe> probes = {
        {100.5, -20.5, 0, 40, "at the very centre: no direction, the world's pull only", {}},
        {108.5, -20.5, -40, 40, "on the circle itself: inside, full strength", {}},
        {108.6, -20.5, 0, 40, "just past the circle: outside", {}},
        {100.5, -12.5, 0, 0, "on the circle below: pulled up as hard as the world pulls down", {}},
        {195.5, -20.5, 80, 40, "inside two points: they add", {}},
        {204.5, -20.5, 0, 40, "between two points: they cancel out", {}},
        {300.5 + 8, -20.5, 0, 40, "fading, on the circle: nothing left", {}},
        {300.5 + 4, -20.5, -20, 40, "fading, halfway: half strength", {}},
        {404.5, -20.5, -20, 0, "replacing: no world pull inside, the other point still adds", {}},
        {396.5, -20.5, 40, 0, "replacing alone: only its own pull", {}},
        {504.5, -20.5, 0, 40, "zero strength: the world's pull only", {}},
        {524.5, -20.5, 0, 0, "zero strength replacing: no pull at all", {}},
        {604.5, -20.5, 40, 40, "negative strength: pushed away", {}},
    };
    for (Probe& p : probes) {
        p.e = game.scene.spawn(scene::Position::at_tile(p.x, p.y));
        REQUIRE(p.e.is_valid());
        sim::Body body;
        body.collide = false;
        p.e.set<sim::Body>(body);
    }
    REQUIRE(game.sim->update(1.0 / 60.0, view) >= 1);
    CHECK(game.sim->gravity().source_count() == 9);
    for (const Probe& p : probes) {
        CAPTURE(p.what);
        const sim::Body& body = p.e.get<sim::Body>();
        CHECK(body.gx == doctest::Approx(p.gx).epsilon(1e-4));
        CHECK(body.gy == doctest::Approx(p.gy).epsilon(1e-4));
    }
    // And over time a body inside a point goes toward its centre.
    flecs::entity e = game.scene.spawn(scene::Position::at_tile(106.5, -20.5));
    sim::Body body;
    body.collide = false;
    body.gravity = 1;
    e.set<sim::Body>(body);
    for (int i = 0; i < 20; ++i) game.sim->update(1.0 / 60.0, view);
    CHECK(e.get<scene::Position>().tile_x() < 106.0);

    // No world pull at all (a top-down level): only the points pull.
    REQUIRE(save_physics(folder, {0, 0}));
    GameRun still(module, folder);
    still.sim->update(0, view);
    still.world.finish_loading();
    still.sim->update(0, view);
    flecs::entity in = still.scene.spawn(scene::Position::at_tile(104.5, -20.5));
    flecs::entity out = still.scene.spawn(scene::Position::at_tile(150.5, -20.5));
    in.set<sim::Body>(body);
    out.set<sim::Body>(body);
    REQUIRE(still.sim->update(1.0 / 60.0, view) >= 1);
    CHECK(in.get<sim::Body>().gx == doctest::Approx(-40));
    CHECK(in.get<sim::Body>().gy == doctest::Approx(0));
    CHECK(out.get<sim::Body>().gx == 0);
    CHECK(out.get<sim::Body>().gy == 0);
}

TEST_CASE("flow trial: water and sand go the world's way, and reset puts the level back as saved") {
    PoolScope pool;
    PhysModule module;
    const fs::path folder = temp_folder("forge_test_level_trial");
    Level level(module);
    REQUIRE(level.open(folder));
    const Rect view{0, -64, 128, 0};
    level.update({&view, 1});
    level.ensure_loaded(view);
    // A pool of water and a heap of sand in the air, as the «Физика» mode pours them.
    TileStroke water(level, "Вода");
    std::vector<Cell> cells;
    rect_cells(10, -30, 14, -28, true, cells);
    CHECK(water.paint(kLiquids, cells, kWater) == 15);
    TileStroke sand(level, "Песок");
    cells.clear();
    rect_cells(40, -30, 41, -29, true, cells);
    CHECK(sand.paint(kBlocks, cells, kSand) == 4);
    REQUIRE(level.save().ok);
    const Rect chunks{0, -1, 2, 0};
    const std::vector<TileId> before = all_tiles(level.world(), chunks);
    const u64 edits = level.edits();

    auto liquid_at = [&](i32 x, i32 y) { return level.tile(kLiquids, x, y); };
    auto run = [&](FlowTrial& t, int ticks) {
        for (int i = 0; i < ticks; ++i) {
            const Rect keep[2] = {view, t.keep()};
            level.update({keep, 2});
            t.update(1.0 / 60.0);
        }
    };
    FlowTrial trial;
    REQUIRE(trial.start(level, view));
    CHECK(trial.running());
    CHECK(trial.down() == 0); // down
    run(trial, 240);
    // The water lies on the stone at y = 0, the sand too.
    CHECK(liquid_at(12, -30) == 0);
    u32 bottom = 0;
    for (i32 x = 0; x < 128; ++x) bottom += sim::liquid_amount(liquid_at(x, -1));
    CHECK(bottom > 0);
    CHECK(level.tile(kBlocks, 40, -30) == 0);
    CHECK(level.tile(kBlocks, 40, -1) == kSand);
    CHECK(level.edits() == edits); // not an edit of the level
    CHECK(trial.reset());
    CHECK_FALSE(trial.running());
    CHECK(all_tiles(level.world(), chunks) == before);
    // Back as saved: nothing to write.
    Level::SaveReport r = level.save();
    CHECK(r.ok);
    CHECK(r.tile_chunks == 0);

    // No world pull: nothing moves.
    level.set_physics({0, 0});
    REQUIRE(trial.start(level, view));
    CHECK(trial.still());
    run(trial, 60);
    CHECK(all_tiles(level.world(), chunks) == before);
    trial.reset();

    // Up and to the left: the water goes that way (the main axis of the pull).
    level.set_physics({-10, -40});
    REQUIRE(trial.start(level, view));
    CHECK(trial.down() == 2); // up
    run(trial, 240);
    CHECK(liquid_at(12, -30) == 0);
    u32 top = 0;
    for (i32 x = 0; x < 128; ++x) top += sim::liquid_amount(liquid_at(x, -64));
    CHECK(top > 0);
    trial.reset();
    CHECK(all_tiles(level.world(), chunks) == before);
    level.set_physics({-40, 10});
    REQUIRE(trial.start(level, view));
    CHECK(trial.down() == 1); // left
    run(trial, 240);
    CHECK(liquid_at(12, -30) == 0);
    u32 left = 0;
    for (i32 y = -64; y < 0; ++y) left += sim::liquid_amount(liquid_at(0, y));
    CHECK(left > 0);
    trial.reset();
    CHECK(all_tiles(level.world(), chunks) == before);

    // A change not saved yet stays not saved after the trial.
    level.set_physics({0, 40});
    REQUIRE(level.set_tile(kBlocks, 70, -40, kSand));
    const std::vector<TileId> edited = all_tiles(level.world(), chunks);
    REQUIRE(trial.start(level, view));
    run(trial, 120);
    CHECK(level.tile(kBlocks, 70, -40) == 0); // it fell
    trial.reset();
    CHECK(all_tiles(level.world(), chunks) == edited);
    r = level.save();
    CHECK(r.ok);
    CHECK(r.tile_chunks == 1);
}
