#include "forge/core/jobs.h"
#include "forge/editor/document.h"
#include "forge/editor/undo.h"
#include "forge/level/level.h"
#include "forge/level/object_edit.h"
#include "forge/level/tile_edit.h"

#include "test_types.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace forge;
using namespace forge::world;
using namespace forge::level;

namespace {

struct PoolScope {
    PoolScope() { jobs::init(3); }
    ~PoolScope() { jobs::shutdown(); }
};

// Air above y = 0, stone below; layer 1 is blocks.
struct FlatGenerator final : Generator {
    void generate(ChunkCoord coord, const ChunkTiles& out) const override {
        TileId* blocks = out.layer(1);
        for (i32 ly = 0; ly < kChunkSize; ++ly) {
            const i32 y = coord.y * kChunkSize + ly;
            for (i32 lx = 0; lx < kChunkSize; ++lx) blocks[ly * kChunkSize + lx] = y >= 0 ? 3 : 0;
        }
    }
};

struct TestModule final : LevelModule {
    std::vector<std::string> layers{"Стены", "Блоки"};
    std::vector<TileDef> palette{{"stone", "Камень", "Земля", "", 1, 3, "1"}, {"sand", "Песок", "Земля", "", 1, 4, "2"}};
    std::string title() const override { return "Проверка"; }
    WorldDesc world_desc() const override {
        WorldDesc d;
        d.layer_count = 2;
        d.load_margin = 0;
        d.keep_extra = 0;
        return d;
    }
    std::shared_ptr<const Generator> generator() const override { return std::make_shared<FlatGenerator>(); }
    void setup_scene(scene::Scene& scene) override { scene.register_component<test::Stats>(); }
    const std::vector<std::string>& layer_names() const override { return layers; }
    const std::vector<TileDef>& tiles() const override { return palette; }
    void tile_icon(const TileDef&, u32 size, std::vector<u8>& rgba) const override { rgba.assign(size * size * 4, 255); }
    void start(f64& x, f64& y) const override { x = y = 0; }
    bool init_view(SDL_GPUDevice*, SDL_GPUTextureFormat) override { return true; }
    void shutdown_view() override {}
    void prepare_view(SDL_GPUCommandBuffer*, Level&, const render::Camera2D&, u32, u32, const ViewOptions&, f64) override {}
    void draw_view(SDL_GPUCommandBuffer*, SDL_GPURenderPass*) override {}
    // One kind of object: anything with Stats, a 1 x 1 box.
    i32 object_kind(flecs::entity e) const override { return e.has<test::Stats>() ? 0 : -1; }
    bool object_box(flecs::entity e, f64& x0, f64& y0, f64& x1, f64& y1) const override {
        const scene::Position& p = e.get<scene::Position>();
        x0 = p.tile_x() - 0.5, x1 = p.tile_x() + 0.5, y0 = p.tile_y() - 0.5, y1 = p.tile_y() + 0.5;
        return true;
    }
};

std::set<std::pair<i32, i32>> as_set(const std::vector<Cell>& cells) {
    std::set<std::pair<i32, i32>> s;
    for (const Cell& c : cells) s.insert({c.x, c.y});
    return s;
}

std::filesystem::path temp_folder(const char* name) {
    const auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(p);
    return p;
}

} // namespace

TEST_CASE("brush shapes cover the expected cells") {
    std::vector<Cell> cells;
    disc_cells(5, 5, 0, cells);
    CHECK(cells.size() == 1);
    cells.clear();
    disc_cells(0, 0, 1, cells);
    CHECK(as_set(cells).size() == 5); // a plus
    cells.clear();
    disc_cells(0, 0, 3, cells);
    CHECK(as_set(cells).size() == cells.size());
    CHECK(as_set(cells).count({3, 0}) == 1);
    CHECK(as_set(cells).count({3, 3}) == 0);

    cells.clear();
    line_cells(0, 0, 10, 3, cells);
    CHECK(cells.size() == 11); // one cell per column on a shallow line
    CHECK(cells.front() == Cell{0, 0});
    CHECK(cells.back() == Cell{10, 3});
    for (usize i = 1; i < cells.size(); ++i) {
        CHECK(std::abs(cells[i].x - cells[i - 1].x) <= 1);
        CHECK(std::abs(cells[i].y - cells[i - 1].y) <= 1);
    }

    cells.clear();
    rect_cells(4, 3, 0, 0, true, cells);
    CHECK(cells.size() == 20);
    cells.clear();
    rect_cells(0, 0, 4, 3, false, cells);
    CHECK(cells.size() == 14);
}

TEST_CASE("a stroke paints, undoes and redoes as one step") {
    PoolScope pool;
    TestModule module;
    Level level(module);
    REQUIRE(level.open({}));
    const Rect view{-40, -40, 40, 40};
    level.update(std::span<const Rect>(&view, 1));
    level.ensure_loaded(view);
    REQUIRE(level.loaded(0, 0));
    CHECK(level.tile(1, 0, -5) == 0);

    editor::Document doc;
    editor::UndoStack history(doc);
    auto stroke = std::make_unique<TileStroke>(level, "Кисть «Песок»");
    // Mouse moves over the same cells twice: they are remembered once.
    stroke->paint_disc(1, 0, -5, 2, 4);
    stroke->paint_disc(1, 1, -5, 2, 4);
    const usize painted = stroke->size();
    CHECK(painted > 13);
    CHECK(level.tile(1, 0, -5) == 4);
    // Cells already holding the value are not part of the stroke.
    TileStroke same(level, "");
    CHECK(same.paint_disc(1, 0, -5, 0, 4) == 0);
    CHECK(same.empty());

    history.execute(std::move(stroke));
    CHECK(history.undo_label() == "Кисть «Песок»");
    CHECK(level.tile(1, 0, -5) == 4);
    REQUIRE(history.undo());
    CHECK(level.tile(1, 0, -5) == 0);
    CHECK(level.tile(1, 2, -5) == 0);
    REQUIRE(history.redo());
    CHECK(level.tile(1, 0, -5) == 4);
    CHECK(level.tile(1, 3, -5) == 4);
}

TEST_CASE("undo reaches a stroke made far from the view") {
    PoolScope pool;
    TestModule module;
    Level level(module);
    REQUIRE(level.open({}));
    const Rect here{-40, -40, 40, 40};
    level.update(std::span<const Rect>(&here, 1));
    level.ensure_loaded(here);

    editor::Document doc;
    editor::UndoStack history(doc);
    auto stroke = std::make_unique<TileStroke>(level, "Кисть");
    stroke->paint_disc(1, 0, 10, 1, 0); // dig into the stone
    history.execute(std::move(stroke));
    CHECK(level.tile(1, 0, 10) == 0);

    // The author flies away; the painted chunk leaves memory.
    const Rect far{5000, -40, 5080, 40};
    for (int i = 0; i < 3; ++i) {
        level.update(std::span<const Rect>(&far, 1));
        level.world().finish_loading();
    }
    REQUIRE(!level.loaded(0, 10));

    REQUIRE(history.undo());
    CHECK(level.loaded(0, 10));
    CHECK(level.tile(1, 0, 10) == 3);
    REQUIRE(history.redo());
    CHECK(level.tile(1, 0, 10) == 0);
}

TEST_CASE("fill stays inside the area and stops at its limit") {
    PoolScope pool;
    TestModule module;
    Level level(module);
    REQUIRE(level.open({}));
    const Rect view{-64, -64, 64, 64};
    level.update(std::span<const Rect>(&view, 1));
    level.ensure_loaded(view);

    // A box of sand with a hollow inside, in the stone.
    std::vector<Cell> cells;
    rect_cells(0, 2, 9, 9, false, cells);
    for (const Cell& c : cells) level.set_tile(1, c.x, c.y, 4);
    cells.clear();
    rect_cells(1, 3, 8, 8, true, cells);
    for (const Cell& c : cells) level.set_tile(1, c.x, c.y, 0);

    std::vector<Cell> fill;
    bool capped = true;
    fill_cells(level, 1, 4, 5, 1'000'000, fill, &capped);
    CHECK(!capped);
    CHECK(fill.size() == 48); // the 8 × 6 hollow and nothing else

    fill.clear();
    fill_cells(level, 1, 4, 5, 10, fill, &capped);
    CHECK(capped);
    CHECK(fill.size() == 10);

    // The open sky reaches the edge of the loaded area and stops there.
    fill.clear();
    fill_cells(level, 1, 0, -10, 1'000'000, fill, &capped);
    CHECK(!capped);
    for (const Cell& c : fill) CHECK(level.loaded(c.x, c.y));
}

TEST_CASE("a level keeps its changes in its folder and a game copies them") {
    PoolScope pool;
    const auto folder = temp_folder("forge_test_level");
    const auto game = temp_folder("forge_test_level_game");
    TestModule module;
    {
        Level level(module);
        REQUIRE(level.open(folder));
        const Rect view{-40, -40, 40, 40};
        level.update(std::span<const Rect>(&view, 1));
        level.ensure_loaded(view);
        TileStroke stroke(level, "Кисть");
        stroke.paint_disc(1, 3, -3, 1, 4);
        const Level::SaveReport r = level.save();
        CHECK(r.ok);
        CHECK(r.tile_chunks >= 1);
    }
    REQUIRE(copy_level(folder, game));
    for (const auto& path : {folder, game}) {
        Level level(module);
        REQUIRE(level.open(path));
        const Rect view{-40, -40, 40, 40};
        level.update(std::span<const Rect>(&view, 1));
        level.ensure_loaded(view);
        CHECK(level.tile(1, 3, -3) == 4);
        CHECK(level.tile(1, 3, -5) == 0);
    }
    CHECK(copy_level({}, game)); // no level folder: nothing to copy
    std::filesystem::remove_all(folder);
    std::filesystem::remove_all(game);
}

TEST_CASE("objects are placed, moved, edited and deleted with undo, and saved") {
    PoolScope pool;
    const auto folder = temp_folder("forge_test_level_objects");
    TestModule module;
    u64 id = 0;
    {
        Level level(module);
        REQUIRE(level.open(folder));
        const Rect view{-40, -40, 40, 40};
        level.update(std::span<const Rect>(&view, 1));
        level.ensure_loaded(view);
        editor::Document doc;
        editor::UndoStack history(doc);

        flecs::entity e = level.scene().spawn(scene::Position::at_tile(2.5, -1.5));
        REQUIRE(e.is_valid());
        e.set<test::Stats>({50, 100, 3});
        ObjectSnapshot snap = snapshot(level, e);
        id = snap.id;
        CHECK(id != 0);
        CHECK(level.find(id) == e);
        level.update(std::span<const Rect>(&view, 1)); // the spatial index picks from
        CHECK(level.pick(2.7, -1.2) == e);
        CHECK(!level.pick(4, -1.5).is_valid());

        // Packing and unpacking makes the same object again.
        const std::vector<u8> bytes = level.scene().pack(e);
        e.destruct();
        CHECK(!level.find(id).is_valid());
        flecs::entity back = level.scene().unpack(bytes);
        REQUIRE(back.is_valid());
        CHECK(level.find(id) == back);
        CHECK(back.get<test::Stats>().health == 50);

        history.execute(std::make_unique<ObjectsCommand>(level, std::vector<ObjectSnapshot>{snap}, false, "Удалить"));
        history.seal();
        CHECK(!level.find(id).is_valid());
        REQUIRE(history.undo());
        REQUIRE(level.find(id).is_valid());
        CHECK(level.find(id).get<test::Stats>().level == 3);

        // A drag: two steps merge into one entry.
        history.execute(std::make_unique<MoveObjects>(level, std::vector<MoveObjects::Move>{{id, 2.5, -1.5, 3.5, -1.5}}, "Двигать"));
        history.execute(std::make_unique<MoveObjects>(level, std::vector<MoveObjects::Move>{{id, 2.5, -1.5, 6.5, -2.5}}, "Двигать"));
        history.seal();
        CHECK(level.find(id).get<scene::Position>().tile_x() == doctest::Approx(6.5));
        REQUIRE(history.undo());
        CHECK(level.find(id).get<scene::Position>().tile_x() == doctest::Approx(2.5));
        REQUIRE(history.redo());

        const std::string before = component_json(level, level.find(id), reflect::type_of<test::Stats>());
        const std::string after = R"({"health":75,"max_health":100,"level":3})";
        history.execute(std::make_unique<SetObjectComponent>(level, id, 6.5, -2.5, reflect::type_of<test::Stats>(), before,
                                                             after, "health", "Здоровье"));
        history.seal();
        CHECK(level.find(id).get<test::Stats>().health == 75);
        REQUIRE(history.undo());
        CHECK(level.find(id).get<test::Stats>().health == 50);
        REQUIRE(history.redo());

        // Undo reaches an object whose chunk has left memory.
        history.execute(std::make_unique<MoveObjects>(level, std::vector<MoveObjects::Move>{{id, 6.5, -2.5, 9.5, -2.5}}, "Двигать"));
        history.seal();
        const Rect far{5000, -40, 5080, 40};
        for (int i = 0; i < 3; ++i) {
            level.update(std::span<const Rect>(&far, 1));
            level.world().finish_loading();
        }
        REQUIRE(!level.loaded(9, -3));
        REQUIRE(history.undo());
        REQUIRE(level.find(id).is_valid());
        CHECK(level.find(id).get<scene::Position>().tile_x() == doctest::Approx(6.5));
        CHECK(level.save().ok);
    }
    Level level(module);
    REQUIRE(level.open(folder));
    const Rect view{-40, -40, 40, 40};
    level.update(std::span<const Rect>(&view, 1));
    level.ensure_loaded(view);
    const flecs::entity e = level.find(id);
    REQUIRE(e.is_valid());
    CHECK(e.get<test::Stats>().health == 75);
    CHECK(e.get<scene::Position>().tile_x() == doctest::Approx(6.5));
    std::filesystem::remove_all(folder);
}
