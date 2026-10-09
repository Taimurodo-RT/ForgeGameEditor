// The level's areas (the «Зоны» mode): named rectangles and the spawn point
// in areas.json, their ids through undo, saves and reopening, bad files told
// and kept, and the rules a game follows with them: who came in and went out
// (edges, overlaps, chunk borders, a new game, a loaded save) and whose music
// plays.

#include "forge/core/file.h"
#include "forge/core/jobs.h"
#include "forge/editor/document.h"
#include "forge/editor/undo.h"
#include "forge/level/areas.h"
#include "forge/level/level.h"

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

struct EmptyGenerator final : Generator {
    void generate(ChunkCoord, const ChunkTiles& out) const override {
        for (u32 l = 0; l < out.layer_count; ++l) std::fill(out.layer(l), out.layer(l) + kChunkSize * kChunkSize, TileId{0});
    }
};

struct AreasModule final : LevelModule {
    std::vector<std::string> layers{"Стены", "Блоки"};
    std::vector<TileDef> palette{{"stone", "Камень", "Земля", "", 1, 3, ""}};
    std::string title() const override { return "Зоны"; }
    WorldDesc world_desc() const override {
        WorldDesc d;
        d.layer_count = 2;
        d.load_margin = 0;
        d.keep_extra = 0;
        return d;
    }
    std::shared_ptr<const Generator> generator() const override { return std::make_shared<EmptyGenerator>(); }
    void setup_scene(scene::Scene&) override {}
    const std::vector<std::string>& layer_names() const override { return layers; }
    const std::vector<TileDef>& tiles() const override { return palette; }
    void tile_icon(const TileDef&, u32 size, std::vector<u8>& rgba) const override { rgba.assign(size * size * 4, 255); }
    void start(f64& x, f64& y) const override { x = y = 0; }
    bool init_view(SDL_GPUDevice*, SDL_GPUTextureFormat) override { return true; }
    void shutdown_view() override {}
    void prepare_view(SDL_GPUCommandBuffer*, Level&, const render::Camera2D&, u32, u32, const ViewOptions&, f64) override {}
    void draw_view(SDL_GPUCommandBuffer*, SDL_GPURenderPass*) override {}
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

Area area(u64 id, const char* name, i32 x0, i32 y0, i32 x1, i32 y1, const char* music = "") {
    return Area{id, name, x0, y0, x1, y1, music};
}

std::string said(const std::vector<AreaEvent>& events) {
    std::string s;
    for (const AreaEvent& e : events) s += (e.entered ? "+" : "-") + std::to_string(e.area) + " ";
    return s;
}

} // namespace

TEST_CASE("areas: ids as text and back, names and music names") {
    CHECK(area_id_text(0x9f2c41d07a5be318ull) == "9f2c41d07a5be318");
    CHECK(area_id_text(1) == "0000000000000001");
    u64 id = 0;
    CHECK(parse_area_id("9f2c41d07a5be318", id));
    CHECK(id == 0x9f2c41d07a5be318ull);
    CHECK(parse_area_id("FFFFFFFFFFFFFFFF", id));
    CHECK(id == ~0ull);
    for (const char* bad : {"", "0", "0000000000000000", "12345678901234567", "xyz", "-1", "1 2"}) {
        CAPTURE(bad);
        CHECK_FALSE(parse_area_id(bad, id));
    }
    CHECK(clean_area_name("  Шахта \t") == "Шахта");
    CHECK(clean_area_name("   ").empty());
    CHECK(valid_music_name("шахта.wav"));
    for (const char* bad : {"", ".", "..", "../шахта.wav", "sounds/шахта.wav", "C:шахта.wav", "a\\b.ogg"}) {
        CAPTURE(bad);
        CHECK_FALSE(valid_music_name(bad));
    }
    CHECK(area_problem(area(1, "Шахта", 0, 0, 10, 10)).empty());
    CHECK_FALSE(area_problem(area(0, "Шахта", 0, 0, 10, 10)).empty());
    CHECK_FALSE(area_problem(area(1, "", 0, 0, 10, 10)).empty());
    CHECK_FALSE(area_problem(area(1, " Шахта", 0, 0, 10, 10)).empty());
    CHECK_FALSE(area_problem(area(1, "Шах\nта", 0, 0, 10, 10)).empty());
    std::string long_name;
    for (int i = 0; i < 60; ++i) long_name += "ш"; // 60 letters, 120 bytes
    CHECK(area_problem(area(1, long_name.c_str(), 0, 0, 10, 10)).empty());
    long_name += "ш";
    CHECK_FALSE(area_problem(area(1, long_name.c_str(), 0, 0, 10, 10)).empty());
    CHECK_FALSE(area_problem(area(1, "Шахта", 0, 0, 0, 10)).empty());
    CHECK_FALSE(area_problem(area(1, "Шахта", 0, 10, 10, 5)).empty());
    CHECK_FALSE(area_problem(area(1, "Шахта", 0, 0, kMaxAreaSide + 1, 10)).empty());
    CHECK(area_problem(area(1, "Шахта", 0, 0, kMaxAreaSide, 10)).empty());
    CHECK_FALSE(area_problem(area(1, "Шахта", 0, 0, 10, 10, "../x.wav")).empty());
}

TEST_CASE("areas.json: none is no areas; set, undone, merged, saved and read back with the same ids") {
    PoolScope pool;
    AreasModule module;
    const fs::path folder = temp_folder("forge_test_level_areas");
    editor::Document doc;
    editor::UndoStack history(doc);
    const u64 mine = 0x9f2c41d07a5be318ull, yard = 0x0123456789abcdefull;
    {
        Level level(module);
        REQUIRE(level.open(folder));
        CHECK(level.areas() == LevelAreas{});
        CHECK(level.areas_error().empty());
        CHECK(level.save().ok);
        CHECK_FALSE(fs::exists(folder / "areas.json")); // nothing changed: no file
        LevelAreas a;
        a.areas.push_back(area(mine, "Шахта", 60, -20, 70, -5, "шахта.wav"));
        history.execute(std::make_unique<SetAreas>(level, level.areas(), a, "Нарисовать зону"));
        history.seal();
        LevelAreas b = a;
        b.areas.push_back(area(yard, "Двор", -10, -30, 10, -10));
        history.execute(std::make_unique<SetAreas>(level, level.areas(), b, "Нарисовать зону"));
        history.seal();
        CHECK(history.size() == 2); // two gestures: two entries
        // A name typed letter by letter: one entry.
        for (const char* name : {"Ш", "Ша", "Шахта у реки"}) {
            LevelAreas c = level.areas();
            c.find(mine)->name = name;
            history.execute(std::make_unique<SetAreas>(level, level.areas(), c, "Имя зоны", "name:" + area_id_text(mine)));
        }
        history.seal();
        CHECK(history.size() == 3);
        CHECK(level.areas().find(mine)->name == "Шахта у реки");
        history.undo();
        CHECK(level.areas().find(mine)->name == "Шахта");
        history.redo();
        CHECK(level.areas().find(mine)->name == "Шахта у реки");
        // The spawn point.
        LevelAreas d = level.areas();
        d.spawn = true;
        d.spawn_x = 12.5;
        d.spawn_y = -3;
        history.execute(std::make_unique<SetAreas>(level, level.areas(), d, "Точка появления"));
        history.seal();
        while (history.can_undo()) history.undo();
        CHECK(level.areas() == LevelAreas{});
        CHECK_FALSE(level.areas_changed());
        while (history.can_redo()) history.redo();
        CHECK(level.areas().areas.size() == 2);
        CHECK(level.areas().find(mine)->id == mine); // undo and redo keep the id
        const Level::SaveReport r = level.save();
        CHECK(r.ok);
        CHECK(r.areas);
        CHECK_FALSE(r.light);
        CHECK_FALSE(level.areas_changed());
        // Saving again writes nothing more.
        CHECK_FALSE(level.save().areas);
    }
    const std::string text = read_text(folder / "areas.json");
    CHECK(text.find("\"id\": \"9f2c41d07a5be318\"") != std::string::npos);
    CHECK(text.find("\"name\": \"Шахта у реки\"") != std::string::npos);
    CHECK(text.find("\"music\": \"шахта.wav\"") != std::string::npos);
    {
        // As the game reads it: a new level from the folder.
        Level level(module);
        REQUIRE(level.open(folder));
        const LevelAreas& a = level.areas();
        REQUIRE(a.areas.size() == 2);
        CHECK(a.areas[0] == area(mine, "Шахта у реки", 60, -20, 70, -5, "шахта.wav"));
        CHECK(a.areas[1] == area(yard, "Двор", -10, -30, 10, -10));
        CHECK(a.spawn);
        CHECK(a.spawn_x == 12.5);
        CHECK(a.spawn_y == -3);
    }
    // No spawn point: none in the file.
    LevelAreas only;
    only.areas.push_back(area(yard, "Двор", 0, 0, 5, 5));
    CHECK(save_areas(folder, only));
    CHECK(read_text(folder / "areas.json").find("spawn") == std::string::npos);
    LevelAreas back;
    CHECK(load_areas(folder, back));
    CHECK(back == only);
    // An area with a problem is not written.
    LevelAreas wrong = only;
    wrong.areas[0].x1 = 0;
    std::string why;
    CHECK_FALSE(save_areas(folder, wrong, &why));
    CHECK(why.find("areas.json") != std::string::npos);
    CHECK(load_areas(folder, back));
    CHECK(back == only);
}

TEST_CASE("areas.json: bad files are told and kept; a new save keeps the old one aside") {
    PoolScope pool;
    AreasModule module;
    const fs::path folder = temp_folder("forge_test_level_areas_bad");
    const char* bad[] = {
        "не JSON",
        "[]",
        R"({"areas": {}})",
        R"({"areas": [{"id": "0", "name": "Шахта", "x0": 0, "y0": 0, "x1": 5, "y1": 5}]})",
        R"({"areas": [{"id": 12, "name": "Шахта", "x0": 0, "y0": 0, "x1": 5, "y1": 5}]})",
        R"({"areas": [{"id": "1", "name": "", "x0": 0, "y0": 0, "x1": 5, "y1": 5}]})",
        R"({"areas": [{"id": "1", "name": "Шахта", "x0": 0, "y0": 0, "x1": 5.5, "y1": 5}]})",
        R"({"areas": [{"id": "1", "name": "Шахта", "x0": 5, "y0": 0, "x1": 5, "y1": 5}]})",
        R"({"areas": [{"id": "1", "name": "Шахта", "x0": 0, "y0": 0, "x1": 5000, "y1": 5}]})",
        R"({"areas": [{"id": "1", "name": "Шахта", "x0": 0, "y0": 0, "x1": 5, "y1": 99999999999}]})",
        R"({"areas": [{"id": "1", "name": "Шахта", "x0": 0, "y0": 0, "x1": 5, "y1": 5, "music": "../a.wav"}]})",
        R"({"areas": [{"id": "1", "name": "А", "x0": 0, "y0": 0, "x1": 5, "y1": 5}, {"id": "1", "name": "Б", "x0": 0, "y0": 0, "x1": 5, "y1": 5}]})",
        R"({"areas": [], "spawn": {"x": "a", "y": 1}})",
        R"({"areas": [], "spawn": {"x": 1e300, "y": 1}})",
    };
    for (const char* text : bad) {
        CAPTURE(text);
        std::error_code ec;
        fs::remove(folder / "areas.broken.json", ec);
        write_text(folder / "areas.json", text);
        Level level(module);
        REQUIRE(level.open(folder));
        CHECK(level.areas() == LevelAreas{});
        CHECK(level.areas_error().find("areas.json") != std::string::npos);
        CHECK(level.save().ok);
        CHECK(read_text(folder / "areas.json") == text); // not overwritten while nothing changed
        LevelAreas a;
        a.areas.push_back(area(7, "Новая", 0, 0, 3, 3));
        level.set_areas(a);
        CHECK(level.save().ok);
        CHECK(level.areas_error().empty());
        CHECK(read_text(folder / "areas.broken.json") == text); // kept aside, not lost
        LevelAreas back;
        CHECK(load_areas(folder, back));
        CHECK(back == a);
    }
}

TEST_CASE("areas.json: a write error is told, nothing is lost, the save is not called done") {
    PoolScope pool;
    AreasModule module;
    const fs::path folder = temp_folder("forge_test_level_areas_write");
    Level level(module);
    REQUIRE(level.open(folder));
    fs::create_directories(folder / "areas.json" / "в пути"); // something in the way
    LevelAreas a;
    a.areas.push_back(area(5, "Шахта", 0, 0, 4, 4));
    level.set_areas(a);
    const Level::SaveReport r = level.save();
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.areas);
    CHECK(r.error.find("areas.json") != std::string::npos);
    CHECK(level.areas_changed());
    CHECK(level.areas() == a);
    fs::remove_all(folder / "areas.json");
    CHECK(level.save().ok);
    LevelAreas back;
    CHECK(load_areas(folder, back));
    CHECK(back == a);
}

TEST_CASE("areas: one across a chunk border stays one through unloading and coming back") {
    PoolScope pool;
    AreasModule module;
    const fs::path folder = temp_folder("forge_test_level_areas_chunks");
    Level level(module);
    REQUIRE(level.open(folder));
    const Rect view{0, -64, 128, 0};
    level.update({&view, 1});
    level.ensure_loaded(view);
    LevelAreas a;
    a.areas.push_back(area(9, "Шахта", 60, -20, 70, -5)); // x = 64 is a chunk border
    level.set_areas(a);
    const u32 before = static_cast<u32>(level.scene().ecs().count<scene::Position>());
    const Rect far{4096, -64, 4224, 0};
    for (int i = 0; i < 3; ++i) level.update({&far, 1});
    CHECK_FALSE(level.loaded(60, -10));
    CHECK(level.areas() == a);
    for (int i = 0; i < 3; ++i) level.update({&view, 1});
    level.ensure_loaded(view);
    CHECK(level.areas() == a);
    CHECK(static_cast<u32>(level.scene().ecs().count<scene::Position>()) == before); // no objects of their own
}

TEST_CASE("area watch: in on the left and top edges, out on the right and bottom; once per entry") {
    LevelAreas a;
    a.areas.push_back(area(1, "Шахта", 10, 20, 20, 30));
    AreaWatch w;
    std::vector<AreaEvent> ev;
    w.step(a, 9.999, 25, ev);
    CHECK(ev.empty());
    w.step(a, 10, 25, ev); // the left edge: in
    CHECK(said(ev) == "+1 ");
    ev.clear();
    for (f64 x = 10; x < 20; x += 0.5) w.step(a, x, 25, ev); // staying: nothing again
    CHECK(ev.empty());
    w.step(a, 20, 25, ev); // the right edge: out
    CHECK(said(ev) == "-1 ");
    ev.clear();
    w.step(a, 19.5, 25, ev); // back: in again
    CHECK(said(ev) == "+1 ");
    ev.clear();
    w.step(a, 15, 30, ev); // the bottom edge: out
    CHECK(said(ev) == "-1 ");
    ev.clear();
    w.step(a, 15, 20, ev); // the top edge: in
    CHECK(said(ev) == "+1 ");
    ev.clear();
    w.step(a, 15, 19.99, ev);
    CHECK(said(ev) == "-1 ");
}

TEST_CASE("area watch: overlapping areas each come and go on their own; a chunk border means nothing") {
    LevelAreas a;
    a.areas.push_back(area(1, "Шахта", 0, 0, 100, 10));
    a.areas.push_back(area(2, "Склад", 50, 0, 70, 10));
    AreaWatch w;
    std::vector<AreaEvent> ev;
    w.step(a, -1, 5, ev);
    for (f64 x = -1; x <= 101; x += 0.25) w.step(a, x, 5, ev); // over x = 64 too
    CHECK(said(ev) == "+1 +2 -2 -1 ");
    ev.clear();
    // Straight into both at once: in list order.
    w.step(a, 60, 5, ev);
    CHECK(said(ev) == "+1 +2 ");
    ev.clear();
    // Out of both at once: left in list order.
    w.step(a, 200, 5, ev);
    CHECK(said(ev) == "-1 -2 ");
    ev.clear();
    // From one into the other directly: out first, then in.
    LevelAreas b;
    b.areas.push_back(area(3, "Левая", 0, 0, 10, 10));
    b.areas.push_back(area(4, "Правая", 10, 0, 20, 10));
    w.clear();
    w.step(b, 9.9, 5, ev);
    CHECK(said(ev) == "+3 ");
    ev.clear();
    w.step(b, 10, 5, ev);
    CHECK(said(ev) == "-3 +4 ");
}

TEST_CASE("area watch: a new game inside comes in at the first step, a loaded save does not") {
    LevelAreas a;
    a.areas.push_back(area(1, "Шахта", 0, 0, 10, 10));
    std::vector<AreaEvent> ev;
    AreaWatch fresh;
    fresh.clear();
    fresh.step(a, 5, 5, ev);
    CHECK(said(ev) == "+1 ");
    ev.clear();
    AreaWatch loaded;
    loaded.settle(a, 5, 5);
    CHECK(loaded.inside(1));
    loaded.step(a, 5, 5, ev);
    CHECK(ev.empty());
    loaded.step(a, 15, 5, ev);
    CHECK(said(ev) == "-1 ");
}

TEST_CASE("area music: the smallest area with music; of equal ones the later; none without") {
    LevelAreas a;
    a.areas.push_back(area(1, "Шахта", 0, 0, 100, 100, "шахта.wav"));
    a.areas.push_back(area(2, "Склад", 40, 40, 60, 60, "склад.wav"));
    a.areas.push_back(area(3, "Тихий угол", 45, 45, 50, 50)); // smallest, but no music
    a.areas.push_back(area(4, "Колодец", 70, 70, 90, 90, "колодец.wav"));
    a.areas.push_back(area(5, "Колодец 2", 70, 70, 90, 90, "колодец 2.wav")); // as big, later
    CHECK(music_area(a, 10, 10)->id == 1);
    CHECK(music_area(a, 50, 50)->id == 2);
    CHECK(music_area(a, 46, 46)->id == 2);
    CHECK(music_area(a, 80, 80)->id == 5);
    CHECK(music_area(a, 100, 50) == nullptr); // the right edge: out
    CHECK(music_area(a, -1, 50) == nullptr);
}
