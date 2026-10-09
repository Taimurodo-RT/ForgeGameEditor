// The level's light (the «Свет» mode): lamps with a radius that ends where it
// says however bright they are, the time of day in light.json, and light
// sources as saved objects of the level. Each check reads the level from its
// folder, as the game does.

#include "forge/core/file.h"
#include "forge/core/jobs.h"
#include "forge/data/json.h"
#include "forge/editor/document.h"
#include "forge/editor/undo.h"
#include "forge/level/level.h"
#include "forge/level/light.h"
#include "forge/level/object_edit.h"
#include "forge/render/lighting.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace forge;
using namespace forge::world;
using namespace forge::level;
using render::LightKind;
using render::PointLight;
namespace fs = std::filesystem;

namespace {

struct PoolScope {
    PoolScope() { jobs::init(3); }
    ~PoolScope() { jobs::shutdown(); }
};

constexpr TileId kStone = 3;
constexpr u32 kBlocks = 1;

// Air above y = 0, stone below.
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

struct LightModule final : LevelModule {
    std::vector<std::string> layers{"Стены", "Блоки"};
    std::vector<TileDef> palette{{"stone", "Камень", "Земля", "", kBlocks, kStone, ""}};
    std::string title() const override { return "Свет"; }
    WorldDesc world_desc() const override {
        WorldDesc d;
        d.layer_count = 2;
        d.load_margin = 0;
        d.keep_extra = 0;
        return d;
    }
    std::shared_ptr<const Generator> generator() const override { return std::make_shared<FlatGenerator>(); }
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

const f32 kTransmit[4] = {0.93f, 0.82f, 0.72f, 0.0f};

// A grid of open cells, one tile each, its corner at 0, 0.
struct Grid {
    u32 w, h;
    std::vector<u32> cells;
    std::vector<f32> rgb;
    Grid(u32 w_, u32 h_) : w(w_), h(h_), cells(static_cast<usize>(w_) * h_, 0) {}
    void set(i32 x, i32 y, LightKind k) { cells[static_cast<usize>(y) * w + static_cast<usize>(x)] = static_cast<u32>(k) << 30; }
    void light(std::initializer_list<PointLight> lamps, u32 step = 1) {
        rgb.clear();
        const std::vector<PointLight> l(lamps);
        render::lamp_light(cells.data(), w, h, 0, 0, step, kTransmit, l, rgb);
    }
    // The red channel of the cell of a tile.
    f32 at(i32 x, i32 y, u32 c = 0) const { return rgb[(static_cast<usize>(y) * w + static_cast<usize>(x)) * 3 + c]; }
};

struct Source {
    u64 id;
    f64 x, y;
    LightSource s;
};
std::vector<Source> sources(Level& level) {
    std::vector<Source> out;
    level.scene().ecs().each([&](const LevelId& id, const scene::Position& p, const LightSource& s) {
        out.push_back({id.id, p.tile_x(), p.tile_y(), s});
    });
    return out;
}

u64 place_source(Level& level, editor::UndoStack& history, f64 x, f64 y, const LightSource& s) {
    flecs::entity e = level.scene().spawn(scene::Position::at_tile(x, y));
    REQUIRE(e.is_valid());
    e.set<LightSource>(s);
    ObjectSnapshot snap = snapshot(level, e);
    const u64 id = snap.id;
    history.execute(std::make_unique<ObjectsCommand>(level, std::vector<ObjectSnapshot>{std::move(snap)}, true, "Поставить"));
    history.seal();
    return id;
}

} // namespace

TEST_CASE("lamp light: the falloff is full at the centre and nothing at the radius") {
    CHECK(render::lamp_falloff(0, 10) == 1.0f);
    CHECK(render::lamp_falloff(5, 10) == doctest::Approx(0.5625));
    CHECK(render::lamp_falloff(9.99, 10) > 0.0f);
    CHECK(render::lamp_falloff(10, 10) == 0.0f);
    CHECK(render::lamp_falloff(11, 10) == 0.0f);
    CHECK(render::lamp_falloff(1, 0) == 0.0f);
    CHECK(render::lamp_falloff(std::nan(""), 10) == 0.0f);
    f32 last = 2;
    for (f64 d = 0; d < 10; d += 0.25) {
        const f32 k = render::lamp_falloff(d, 10);
        CHECK(k < last);
        last = k;
    }
}

TEST_CASE("lamp light: in the open it reaches every cell nearer than the radius and none farther") {
    Grid g(100, 100);
    const PointLight lamp{50.5, 50.5, 1.0f, 0.5f, 0.25f, 10};
    g.light({lamp});
    CHECK(g.at(50, 50, 0) == 1.0f); // the centre: the colour as it is
    CHECK(g.at(50, 50, 1) == 0.5f);
    CHECK(g.at(50, 50, 2) == 0.25f);
    CHECK(g.at(55, 50) == doctest::Approx(0.5625)); // 5 tiles: (1 − ¼)²
    CHECK(g.at(45, 50) == g.at(55, 50));
    CHECK(g.at(50, 55) == g.at(55, 50));
    // A circle, not the octagon the steps of the grid make: every cell whose
    // centre is nearer than 10 has light, every one at 10 or farther none.
    u32 lit = 0, wrong = 0;
    for (i32 y = 30; y <= 70; ++y)
        for (i32 x = 30; x <= 70; ++x) {
            const f64 d = std::hypot(x + 0.5 - lamp.x, y + 0.5 - lamp.y);
            const bool has = g.at(x, y) > 0;
            lit += has;
            if (has != (d < 10)) ++wrong;
            if (d < 10) CHECK(g.at(x, y) == doctest::Approx(render::lamp_falloff(d, 10)).epsilon(1e-5));
        }
    CHECK(wrong == 0);
    CHECK(lit == 305); // the cells of a circle of radius 10 round a cell's centre
    CHECK(g.at(59, 54) > 0); // 9.85 away: lit (the grid's steps alone would count 10.66)
    CHECK(g.at(56, 58) == 0.0f); // 6, 8: exactly 10
}

TEST_CASE("lamp light: brightness, radius and colour do not stand in for each other") {
    Grid dim(120, 120), bright(120, 120), wide(120, 120), blue(120, 120);
    dim.light({{60.5, 60.5, 1, 1, 1, 12}});
    bright.light({{60.5, 60.5, 4, 4, 4, 12}});
    wide.light({{60.5, 60.5, 1, 1, 1, 24}});
    blue.light({{60.5, 60.5, 0, 0, 1, 12}});
    for (i32 x = 60; x < 120; ++x) {
        CAPTURE(x);
        // Four times as bright: four times the light, exactly as far.
        CHECK((dim.at(x, 60) > 0) == (bright.at(x, 60) > 0));
        CHECK(bright.at(x, 60) == doctest::Approx(dim.at(x, 60) * 4).epsilon(1e-5));
        // The colour: only the channels.
        CHECK(blue.at(x, 60, 0) == 0.0f);
        CHECK(blue.at(x, 60, 2) == dim.at(x, 60, 2));
    }
    CHECK(dim.at(71, 60) > 0); // 11 tiles
    CHECK(dim.at(72, 60) == 0.0f); // 12
    // Twice the radius: as bright at the centre, twice as far.
    CHECK(wide.at(60, 60) == dim.at(60, 60));
    CHECK(wide.at(83, 60) > 0);
    CHECK(wide.at(84, 60) == 0.0f);
}

TEST_CASE("lamp light: walls take light away, unloaded places stop it, corners make the way longer") {
    // A wall of rock one tile thick 3 tiles right of the lamp.
    Grid open(80, 80), rock(80, 80), shut(80, 80);
    for (i32 y = 0; y < 80; ++y) {
        rock.set(43, y, LightKind::Solid);
        shut.set(43, y, LightKind::Opaque);
    }
    const PointLight lamp{40.5, 40.5, 1, 1, 1, 15};
    open.light({lamp});
    rock.light({lamp});
    shut.light({lamp});
    CHECK(rock.at(42, 40) == open.at(42, 40)); // before the wall: as in the open
    CHECK(rock.at(44, 40) > 0); // light goes a little through rock
    CHECK(rock.at(44, 40) < open.at(44, 40));
    // 12 tiles straight, the one of rock counting 4.5: 15.5, past 15; 11: 14.5, not yet.
    CHECK(rock.at(52, 40) == 0.0f);
    CHECK(open.at(52, 40) > 0);
    CHECK(rock.at(51, 40) > 0);
    for (i32 x = 43; x < 60; ++x) CHECK(shut.at(x, 40) == 0.0f); // nothing through what is not loaded
    // A gap in the closed wall: light comes through it and round the corner,
    // weaker than it would be straight.
    shut.set(43, 43, LightKind::Open);
    shut.light({lamp});
    CHECK(shut.at(44, 43) > 0);
    CHECK(shut.at(44, 40) > 0); // round the corner: about 7.7 tiles of way, inside 15
    CHECK(shut.at(44, 40) < open.at(44, 40));
    CHECK(shut.at(44, 31) == 0.0f); // the way round is longer than 15, though only 9.8 straight
    // Water: dimmer than air, brighter than rock.
    Grid water(80, 80);
    for (i32 y = 0; y < 80; ++y) water.set(43, y, LightKind::Dense);
    water.light({lamp});
    CHECK(water.at(44, 40) < open.at(44, 40));
    CHECK(water.at(44, 40) > rock.at(44, 40));
}

TEST_CASE("lamp light: two lamps, the brightest wins per channel; a large radius is kept to 40 and does not hang") {
    Grid g(200, 200);
    g.light({{80.5, 100.5, 1, 0.2f, 0.2f, 10}, {88.5, 100.5, 0.2f, 0.3f, 1, 6}});
    CHECK(g.at(80, 100, 0) == 1.0f);
    CHECK(g.at(88, 100, 2) == 1.0f);
    CHECK(g.at(84, 100, 0) == doctest::Approx(render::lamp_falloff(4, 10)));
    CHECK(g.at(84, 100, 2) == doctest::Approx(render::lamp_falloff(4, 6)));
    CHECK(g.at(84, 100, 1) == doctest::Approx(std::max(0.2f * render::lamp_falloff(4, 10), 0.3f * render::lamp_falloff(4, 6))));

    // A radius past 40 is 40: no light past it, the work bounded.
    const auto t0 = std::chrono::steady_clock::now();
    Grid big(200, 200);
    big.light({{100.5, 100.5, 1, 1, 1, 1e9f}});
    const f64 ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(big.at(139, 100) > 0);
    CHECK(big.at(140, 100) == 0.0f);
    CHECK(big.at(100, 141) == 0.0f);
    CHECK(ms < 2000);
    // A lamp off the grid, or with no radius, or not finite: none of this light.
    Grid none(50, 50);
    none.light({{-5, 10, 1, 1, 1, 8}, {10.5, 10.5, 1, 1, 1, 0}, {std::nan(""), 10, 1, 1, 1, 8}});
    bool dark = true;
    for (f32 v : none.rgb) dark = dark && v == 0;
    CHECK(dark);
    // A coarser grid (zoomed out, 2 tiles a cell): the same radius in tiles.
    Grid coarse(100, 100);
    coarse.light({{100.5, 100.5, 1, 1, 1, 10}}, 2);
    CHECK(coarse.at(50, 50) > 0);
    CHECK(coarse.at(54, 50) > 0);   // cell centre 109: 8.5 tiles
    CHECK(coarse.at(55, 50) == 0.0f); // 111: 10.5
}

TEST_CASE("sky through a day: noon is the light of before, evenings in between, across midnight") {
    const Color noon{1.0f, 0.98f, 0.92f, 1.0f}, night{0.1f, 0.12f, 0.22f, 1.0f}, dusk{0.9f, 0.5f, 0.3f, 1.0f};
    const render::SkyKey keys[] = {{5, night}, {8, noon}, {17, noon}, {19, dusk}, {21, night}};
    CHECK(render::sky_at(keys, 12, {}) == noon);
    CHECK(render::sky_at(keys, 8, {}) == noon);
    CHECK(render::sky_at(keys, 36, {}) == noon); // a day later
    CHECK(render::sky_at(keys, 2, {}) == night);
    CHECK(render::sky_at(keys, 23.5, {}) == night); // across midnight, from 21 to 5
    const Color half = render::sky_at(keys, 18, {});
    CHECK(half.r == doctest::Approx(0.95));
    CHECK(half.b == doctest::Approx(0.61));
    CHECK(render::sky_at({}, 18, noon) == noon);
    CHECK(render::sky_at(keys, std::nan(""), dusk) == dusk);
}

TEST_CASE("light.json: none is noon; set, undone, saved and read back; bad ones are told and kept") {
    PoolScope pool;
    LightModule module;
    const fs::path folder = temp_folder("forge_test_level_light");
    editor::Document doc;
    editor::UndoStack history(doc);
    {
        Level level(module);
        REQUIRE(level.open(folder));
        CHECK(level.light() == LevelLight{12});
        CHECK(level.light_error().empty());
        CHECK(level.save().ok);
        CHECK_FALSE(fs::exists(folder / "light.json")); // nothing changed: no file
        history.execute(std::make_unique<SetLight>(level, level.light(), LevelLight{19}, "время"));
        history.execute(std::make_unique<SetLight>(level, level.light(), LevelLight{21.5f}, "время"));
        CHECK(history.size() == 1); // a slider's drag: one entry
        history.undo();
        CHECK(level.light() == LevelLight{12});
        CHECK_FALSE(level.light_changed());
        history.redo();
        const Level::SaveReport r = level.save();
        CHECK(r.ok);
        CHECK(r.light);
        CHECK_FALSE(r.physics);
        CHECK_FALSE(level.light_changed());
    }
    {
        Level level(module);
        REQUIRE(level.open(folder));
        CHECK(level.light() == LevelLight{21.5f});
    }
    for (const f32 t : {0.0f, 23.99f, 6.25f}) {
        CHECK(save_light(folder, {t}));
        LevelLight back{1};
        CHECK(load_light(folder, back));
        CHECK(back.time == t);
    }
    std::string why;
    for (const f32 t : {24.0f, -0.5f, std::nanf(""), INFINITY}) {
        CHECK_FALSE(valid_light({t}));
        CHECK_FALSE(save_light(folder, {t}, &why));
    }
    CHECK(why.find("light.json") != std::string::npos);

    const char* bad[] = {"не JSON", R"({"time": "вечер"})", R"({"time": 24})", R"({"time": -1})", R"({"time": 1e999})"};
    for (const char* text : bad) {
        CAPTURE(text);
        write_text(folder / "light.json", text);
        Level level(module);
        REQUIRE(level.open(folder));
        CHECK(level.light() == LevelLight{12});
        CHECK(level.light_error().find("light.json") != std::string::npos);
        CHECK(level.save().ok);
        CHECK(read_text(folder / "light.json") == text); // not overwritten
        level.set_light({20});
        CHECK(level.save().ok);
        CHECK(level.light_error().empty());
        LevelLight back;
        CHECK(load_light(folder, back));
        CHECK(back == LevelLight{20});
    }
}

TEST_CASE("light.json: a write error is told, nothing is lost, the save is not called done") {
    PoolScope pool;
    LightModule module;
    const fs::path folder = temp_folder("forge_test_level_light_write");
    Level level(module);
    REQUIRE(level.open(folder));
    fs::create_directories(folder / "light.json"); // something in the way
    level.set_light({3});
    const Level::SaveReport r = level.save();
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("light.json") != std::string::npos);
    CHECK(level.light_changed());
    CHECK(level.light() == LevelLight{3});
    fs::remove_all(folder / "light.json");
    CHECK(level.save().ok);
    LevelLight back;
    CHECK(load_light(folder, back));
    CHECK(back == LevelLight{3});
}

TEST_CASE("light sources: saved with their ids, through unloading, a move over a chunk border and deletes") {
    PoolScope pool;
    LightModule module;
    const fs::path folder = temp_folder("forge_test_level_lights");
    editor::Document doc;
    editor::UndoStack history(doc);
    u64 a = 0, b = 0;
    const LightSource red{1, 0.2f, 0.1f, 2, 12}, blue{0.2f, 0.4f, 1, 1, 6};
    {
        Level level(module);
        REQUIRE(level.open(folder));
        const Rect view{0, -64, 128, 0};
        level.update({&view, 1});
        level.ensure_loaded(view);
        a = place_source(level, history, 60.5, -20.5, red);
        b = place_source(level, history, 20.5, -30.5, blue);
        CHECK(sources(level).size() == 2);
        CHECK(level.pick(60.5, -20.5) == flecs::entity()); // not an object of the palette: only «Свет» picks it
        history.execute(std::make_unique<MoveObjects>(level, std::vector<MoveObjects::Move>{{a, 60.5, -20.5, 70.5, -20.5}}, "Передвинуть"));
        history.seal();
        flecs::entity e = level.find(a);
        REQUIRE(e.is_valid());
        const reflect::TypeInfo* type = reflect::type_of<LightSource>();
        const std::string before = component_json(level, e, type);
        LightSource s = e.get<LightSource>();
        s.radius = 20;
        history.execute(std::make_unique<SetObjectComponent>(level, a, 70.5, -20.5, type, before, data::to_json(type, &s, false), "radius", "Радиус"));
        history.seal();
        const Rect far{4096, -64, 4224, 0};
        for (int i = 0; i < 3; ++i) level.update({&far, 1});
        CHECK_FALSE(level.find(a).is_valid());
        for (int i = 0; i < 3; ++i) level.update({&view, 1});
        level.ensure_loaded(view);
        std::vector<Source> got = sources(level);
        REQUIRE(got.size() == 2);
        for (const Source& x : got) {
            if (x.id == a) {
                CHECK(x.x == 70.5);
                CHECK(x.s.radius == 20);
                CHECK(x.s.brightness == 2);
                CHECK(x.s.r == 1);
            } else {
                CHECK(x.id == b);
                CHECK(x.s.radius == 6);
            }
        }
        // Undo all the way and back: never two of one.
        while (history.can_undo()) history.undo();
        CHECK(sources(level).empty());
        while (history.can_redo()) history.redo();
        CHECK(sources(level).size() == 2);
        history.execute(std::make_unique<ObjectsCommand>(level, std::vector<ObjectSnapshot>{snapshot(level, level.find(b))}, false, "Удалить"));
        CHECK(sources(level).size() == 1);
        history.undo();
        CHECK(sources(level).size() == 2);
        CHECK(level.save().ok);
    }
    // As the game reads it: a new world and scene from the folder.
    Level again(module);
    REQUIRE(again.open(folder));
    const Rect view{0, -64, 128, 0};
    again.update({&view, 1});
    again.ensure_loaded(view);
    std::vector<Source> got = sources(again);
    REQUIRE(got.size() == 2);
    u32 found = 0;
    for (const Source& x : got) found += (x.id == a && x.x == 70.5 && x.s.radius == 20) + (x.id == b && x.s.b == 1);
    CHECK(found == 2);
}

TEST_CASE("light sources: the lamp a source gives keeps its values within the limits") {
    const render::PointLight l = point_light(3.5, -2.5, {1, 0.5f, 0.25f, 2, 10});
    CHECK(l.x == 3.5);
    CHECK(l.y == -2.5);
    CHECK(l.r == 2);
    CHECK(l.g == 1);
    CHECK(l.b == 0.5f);
    CHECK(l.radius == 10);
    const render::PointLight big = point_light(0, 0, {5, -1, 1, 100, 1000});
    CHECK(big.r == kMaxBrightness);
    CHECK(big.g == 0);
    CHECK(big.radius == kMaxLightRadius);
    CHECK(point_light(0, 0, {1, 1, 1, 1, 0}).radius == kMinLightRadius);
    const render::PointLight off = point_light(0, 0, {1, 1, 1, std::nanf(""), 8});
    CHECK(off.r == 0);
    CHECK(off.g == 0);
    CHECK(off.b == 0);
    CHECK(point_light(0, 0, {1, 1, 1, 0, 8}).r == 0); // brightness 0: no light
}

TEST_CASE("clock: hours as text and back") {
    CHECK(clock_text(18.5) == "18:30");
    CHECK(clock_text(0) == "0:00");
    CHECK(clock_text(23.999) == "0:00"); // rounds to the minute, past midnight
    CHECK(clock_text(6.25) == "6:15");
    f64 h = -1;
    CHECK(parse_clock("18:30", h));
    CHECK(h == 18.5);
    CHECK(parse_clock(" 7:05 ", h));
    CHECK(h == doctest::Approx(7 + 5 / 60.0));
    CHECK(parse_clock("18,5", h));
    CHECK(h == 18.5);
    CHECK(parse_clock("24", h));
    CHECK(h == 24); // a number; the range is the caller's
    for (const char* bad : {"", "abc", "18:3", "18:60", "-1:00", "nan", "inf", "1e999", "18:30:00", "7 часов"}) {
        CAPTURE(bad);
        CHECK_FALSE(parse_clock(bad, h));
    }
}
