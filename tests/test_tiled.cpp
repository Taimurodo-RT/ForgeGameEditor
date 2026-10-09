// Maps of Tiled: read as Tiled reads them (its own reading of the example map,
// exported by Tiled, is compared cell for cell and object for object), the
// same map in every layer format Tiled writes, maps that are refused and why,
// what the import makes of the example, and a second import of the same map.
// The maps are in games/examples/tiled and tests/data/tiled (made by
// tests/data/tiled/make.py and re-saved by Tiled 1.8.2).

#include "forge/assets/image.h"
#include "forge/core/file.h"
#include "forge/core/path.h"
#include "forge/level/tiled.h"
#include "forge/level/tiled_import.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace forge;
using namespace forge::level;
using namespace forge::level::tiled;
namespace fs = std::filesystem;

namespace {

fs::path source_dir() { return utf8_path(FORGE_SOURCE_DIR); }
fs::path example_dir() { return source_dir() / "games" / "examples" / "tiled" / utf8_path("Карта Tiled"); }
fs::path example_map() { return example_dir() / utf8_path("уровень.tmx"); }
fs::path test_map(const char* name) { return source_dir() / "tests" / "data" / "tiled" / utf8_path(name); }

std::string read_text(const fs::path& file) {
    std::vector<u8> bytes;
    if (!read_file(file, bytes)) return {};
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void write_text(const fs::path& file, const std::string& text) {
    REQUIRE(write_file_atomic(file, {reinterpret_cast<const u8*>(text.data()), text.size()}));
}

const Layer* layer_named(const Map& m, std::string_view name) {
    for (const Layer& l : m.layers)
        if (l.name == name) return &l;
    return nullptr;
}

const Object* object_with(const Map& m, u32 id) {
    for (const Layer& l : m.layers)
        for (const Object& o : l.objects)
            if (o.id == id) return &o;
    return nullptr;
}

// Every cell of a layer, by (x, y).
std::map<std::pair<i32, i32>, u32> cells_of(const Layer& l) {
    std::map<std::pair<i32, i32>, u32> out;
    for (const Chunk& c : l.chunks)
        for (u32 y = 0; y < c.h; ++y)
            for (u32 x = 0; x < c.w; ++x)
                if (const u32 g = c.gids[static_cast<usize>(y) * c.w + x]) out[{c.x + static_cast<i32>(x), c.y + static_cast<i32>(y)}] = g;
    return out;
}

const char* shape_word(Object::Shape s) {
    switch (s) {
    case Object::Shape::Rect: return "rect";
    case Object::Shape::Point: return "point";
    case Object::Shape::Ellipse: return "ellipse";
    case Object::Shape::Polygon: return "polygon";
    case Object::Shape::Polyline: return "polyline";
    case Object::Shape::Text: return "text";
    case Object::Shape::Capsule: return "capsule";
    }
    return "?";
}

std::string number(f64 v) {
    std::ostringstream s;
    s << v;
    return s.str();
}

bool has_note(const std::vector<Note>& notes, std::string_view part, u64* count = nullptr) {
    for (const Note& n : notes)
        if (n.what.find(part) != std::string::npos) {
            if (count) *count = n.count;
            return true;
        }
    return false;
}

std::string notes_text(const std::vector<Note>& notes) {
    std::string s;
    for (const Note& n : notes) s += n.what + " — " + std::to_string(n.count) + "\n";
    return s;
}

const OwnTile* tile_named(const LevelTiles& t, std::string_view name) {
    for (const OwnTile& o : t.tiles)
        if (o.name == name) return &o;
    return nullptr;
}

std::vector<u8> picture_of(const LevelTiles& t, world::TileId id) {
    for (usize i = 0; i < t.tiles.size(); ++i)
        if (t.tiles[i].id == id) return {t.picture(i), t.picture(i) + t.px * t.px * 4};
    return {};
}

// A tile of the atlas «Земля и камень» (16 px, margin 1, spacing 2, 8 in a row), as drawn.
std::vector<u8> ground_tile(u32 local) {
    std::vector<u8> bytes;
    REQUIRE(read_file(example_dir() / utf8_path("наборы/Земля и камень.png"), bytes));
    assets::CookedTexture img;
    REQUIRE(assets::decode_image(bytes, img));
    const u32 x0 = 1 + (local % 8) * 18, y0 = 1 + (local / 8) * 18;
    std::vector<u8> out;
    for (u32 y = 0; y < 16; ++y)
        for (u32 x = 0; x < 16; ++x)
            for (u32 c = 0; c < 4; ++c) out.push_back(img.rgba8[((y0 + y) * img.width + x0 + x) * 4 + c]);
    return out;
}

// The picture turned by hand, the way Tiled's flags say: the diagonal first, then the two mirrors.
std::vector<u8> turn(const std::vector<u8>& p, u32 flags) {
    std::vector<u8> out(p.size());
    for (u32 y = 0; y < 16; ++y)
        for (u32 x = 0; x < 16; ++x) {
            u32 sx = x, sy = y;
            if (flags & kFlipH) sx = 15 - sx;
            if (flags & kFlipV) sy = 15 - sy;
            if (flags & kFlipD) std::swap(sx, sy);
            std::copy_n(&p[(sy * 16 + sx) * 4], 4, &out[(y * 16 + x) * 4]);
        }
    return out;
}

Plan plan_of(const Map& m, const LevelTiles& own = {}, const LevelAreas& areas = {}, const Record& before = {}) {
    Plan p;
    std::string why;
    Options o;
    REQUIRE_MESSAGE(plan(m, o, own, areas, before, p, &why), why);
    return p;
}

fs::path copy_of_example(const char* name) {
    const fs::path dir = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::copy(example_dir(), dir, fs::copy_options::recursive, ec);
    REQUIRE_MESSAGE(!ec, ec.message());
    return dir;
}

} // namespace

TEST_CASE("Tiled: the example map reads as Tiled itself reads it, cell for cell and object for object") {
    Map m;
    std::string why;
    REQUIRE_MESSAGE(read_map(example_map(), m, &why), why);
    CHECK(m.infinite);
    CHECK(m.orientation == "orthogonal");
    CHECK(m.tile_w == 16);
    CHECK(m.tile_h == 16);
    CHECK(m.background == "#78aee6");

    // Tiled's own reading (its JSON export, templates detached), line by line.
    std::istringstream lines(read_text(test_map("уровень как его читает Tiled.txt")));
    std::string line;
    usize layers_seen = 0, objects_seen = 0;
    std::map<std::string, std::map<std::pair<i32, i32>, u32>> tiled_cells;
    while (std::getline(lines, line)) {
        CAPTURE(line.substr(0, 80));
        std::vector<std::string> f;
        std::string rest = line.substr(line.find(' ') + 1);
        for (usize at = 0;;) {
            const usize bar = rest.find('|', at);
            f.push_back(rest.substr(at, bar == std::string::npos ? std::string::npos : bar - at));
            if (bar == std::string::npos) break;
            at = bar + 1;
        }
        if (line.starts_with("layer ")) {
            ++layers_seen;
            REQUIRE(f.size() == 4);
            const Layer* l = layer_named(m, f[0]);
            REQUIRE(l);
            CHECK(l->kind == Layer::Kind::Tiles);
            CHECK(l->visible == (f[1] == "1"));
            std::istringstream box(f[2]), gids(f[3]);
            i32 x, y;
            u32 w, h;
            box >> x >> y >> w >> h;
            u32 g;
            for (u32 i = 0; gids >> g; ++i)
                if (g) tiled_cells[f[0]][{x + static_cast<i32>(i % w), y + static_cast<i32>(i / w)}] = g;
        } else {
            ++objects_seen;
            REQUIRE(f.size() == 5);
            const Object* o = object_with(m, static_cast<u32>(std::stoul(f[0])));
            REQUIRE(o);
            CHECK(o->name == f[1]);
            CHECK(shape_word(o->shape) == f[2]);
            CHECK(number(o->x) + " " + number(o->y) + " " + number(o->w) + " " + number(o->h) == f[3]);
            CHECK(std::to_string(o->gid) == f[4]);
        }
    }
    CHECK(layers_seen >= 15);
    CHECK(objects_seen == 10);
    for (const auto& [name, cells] : tiled_cells) {
        CAPTURE(name);
        CHECK(cells_of(*layer_named(m, name)) == cells);
    }
    // The layers, groups flattened, in drawing order.
    std::vector<std::string> names;
    for (const Layer& l : m.layers) names.push_back(l.name);
    CHECK(names == std::vector<std::string>{"Небо", "Фон", "Украшения/Декор", "Земля", "Подсказки/Скрытое", "Объекты"});
    CHECK(is_true(layer_named(m, "Земля")->props, "solid"));
    CHECK_FALSE(layer_named(m, "Подсказки/Скрытое")->visible);
    // Chunks at negative x and y.
    const auto sky = cells_of(*layer_named(m, "Небо"));
    CHECK(sky.at({-7, -3}) == 17);
    CHECK(sky.at({-6, -3}) == 18);
    CHECK(sky.at({22, 4}) == (18u | kFlipH));
    // The ore in all eight turns.
    const auto ground = cells_of(*layer_named(m, "Земля"));
    const u32 turns[8] = {0, kFlipH, kFlipV, kFlipH | kFlipV, kFlipD, kFlipD | kFlipH, kFlipD | kFlipV, kFlipD | kFlipH | kFlipV};
    for (i32 i = 0; i < 8; ++i) CHECK(ground.at({2 + i, 16}) == (8u | turns[i]));

    // The tilesets.
    REQUIRE(m.tilesets.size() == 4);
    const Tileset& rock = m.tilesets[0];
    CHECK(rock.firstgid == 1);
    CHECK(rock.source == "наборы/Земля и камень.tsx");
    CHECK(fs::is_regular_file(rock.file));
    CHECK(rock.missing.empty());
    CHECK(rock.name == "Земля и камень");
    CHECK(rock.margin == 1);
    CHECK(rock.spacing == 2);
    CHECK(rock.columns == 8);
    CHECK(rock.count == 16);
    CHECK(fs::is_regular_file(rock.image));
    CHECK(rock.tile(7)->type == "Руда");
    CHECK(rock.tile(7)->collision);
    CHECK_FALSE(rock.tile(8)->collision);
    const Tileset& sky_set = m.tilesets[1];
    CHECK(sky_set.firstgid == 17);
    CHECK(sky_set.source.empty());
    CHECK(sky_set.trans);
    CHECK((sky_set.trans_rgb[0] == 255 && sky_set.trans_rgb[1] == 0 && sky_set.trans_rgb[2] == 255));
    CHECK(m.tilesets[2].firstgid == 21);
    const Tileset& things = m.tilesets[3];
    CHECK(things.firstgid == 25);
    CHECK(things.collection());
    CHECK(things.has(0));
    CHECK_FALSE(things.has(1));
    CHECK(things.has(3));
    CHECK(things.has(7));
    CHECK(things.tile(7)->image_h == 32);
    // The lantern of the template: its tile in the map's tileset of the same file.
    u32 local = 0;
    CHECK(tileset_of(m, object_with(m, 6)->gid, local) == &things);
    CHECK(local == 7);
    CHECK(object_with(m, 6)->template_source == "шаблоны/фонарь.tx");
    CHECK(object_with(m, 6)->template_missing.empty());
    CHECK(object_with(m, 8)->name == "Фонарь у обрыва");
    const Property* music = find_property(object_with(m, 2)->props, "music");
    REQUIRE(music);
    CHECK(music->type == "file");
    CHECK(music->value == "музыка/пещера.wav");
}

TEST_CASE("Tiled: the same map in every layer format Tiled writes is the same map") {
    Map csv;
    std::string why;
    REQUIRE(read_map(example_map(), csv, &why));
    for (const char* name : {"формат base64.tmx", "формат zlib.tmx", "формат gzip.tmx", "формат XML.tmx"}) {
        CAPTURE(name);
        Map m;
        REQUIRE_MESSAGE(read_map(test_map(name), m, &why), why);
        REQUIRE(m.layers.size() == csv.layers.size());
        for (usize i = 0; i < m.layers.size(); ++i) {
            CAPTURE(m.layers[i].name);
            CHECK(m.layers[i].name == csv.layers[i].name);
            CHECK(cells_of(m.layers[i]) == cells_of(csv.layers[i]));
        }
        // Its tilesets are the example's, by a path from elsewhere.
        CHECK(m.tilesets[0].missing.empty());
        CHECK(fs::equivalent(m.tilesets[0].file, csv.tilesets[0].file));
        CHECK(object_with(m, 6)->template_missing.empty());
    }
}

TEST_CASE("Tiled: maps that cannot be read, or not imported, are refused with the reason") {
    struct Case {
        const char* file;
        const char* why;
        bool reads; // read, but refused by the import
    };
    for (const Case& c : {Case{"zstd.tmx", "zstd", false}, Case{"битый base64.tmx", "base64", false}, Case{"не XML.tmx", "XML", false},
                          Case{"набор JSON.tmx", "JSON", false}, Case{"мало клеток.tmx", "клеток 11, а должно быть 12", false},
                          Case{"гексы.tmx", "hexagonal", true}, Case{"клетки 16x8.tmx", "квадратные", true},
                          Case{"номер без набора.tmx", "firstgid", true}}) {
        CAPTURE(c.file);
        Map m;
        std::string why;
        const bool read = read_map(test_map(c.file), m, &why);
        CHECK(read == c.reads);
        if (read) {
            Plan p;
            CHECK_FALSE(plan(m, {}, {}, {}, {}, p, &why));
        }
        CAPTURE(why);
        CHECK(why.find(c.why) != std::string::npos);
    }
    Map m;
    std::string why;
    CHECK_FALSE(read_map(test_map("нет такой.tmx"), m, &why));
    CHECK(why.find("не читается") != std::string::npos);
}

TEST_CASE("Tiled: a finite map, a layer moved by whole cells, a mirrored tile") {
    Map m;
    std::string why;
    REQUIRE_MESSAGE(read_map(test_map("конечная.tmx"), m, &why), why);
    CHECK_FALSE(m.infinite);
    REQUIRE(m.layers.size() == 1);
    REQUIRE(m.layers[0].chunks.size() == 1);
    CHECK(m.layers[0].offset_x == 32);
    CHECK(m.layers[0].offset_y == -16);
    const Plan p = plan_of(m);
    // 4 × 3 cells moved 2 right and 1 up; the map's own rectangle too.
    CHECK(p.x0 == 0);
    CHECK(p.y0 == -1);
    CHECK(p.x1 == 6);
    CHECK(p.y1 == 3);
    CHECK(p.at(0, 2, -1) != 0);  // «Цветок» at the layer's 0, 0
    CHECK(p.at(0, 5, -1) != 0);  // «Мухомор»
    CHECK(p.at(0, 3, 0) != 0);   // «Лоза»
    CHECK(p.at(0, 5, 1) != 0);   // «Мох», mirrored
    CHECK(p.at(0, 0, 0) == 0);
    CHECK(p.at(1, 2, -1) == 0);  // nothing on the blocks
    CHECK(p.tiles.tiles.size() == 4);
    CHECK(tile_named(p.tiles, "Мох ↔"));
    CHECK(p.skipped.empty());
    CHECK(p.missing.empty());
}

TEST_CASE("Tiled: what the import makes of the example map") {
    Map m;
    std::string why;
    REQUIRE(read_map(example_map(), m, &why));
    CHECK(default_target(m, *layer_named(m, "Небо")) == Target::Walls);
    CHECK(default_target(m, *layer_named(m, "Фон")) == Target::Walls);
    CHECK(default_target(m, *layer_named(m, "Украшения/Декор")) == Target::Walls);
    CHECK(default_target(m, *layer_named(m, "Земля")) == Target::Blocks);
    CHECK(default_target(m, *layer_named(m, "Подсказки/Скрытое")) == Target::Skip);
    const Plan p = plan_of(m);
    CAPTURE(notes_text(p.skipped));
    CAPTURE(notes_text(p.missing));
    CHECK(p.px == 16);
    // The chunks the layers that come over have: x -16…48, y -16…32.
    CHECK(p.x0 == -16);
    CHECK(p.y0 == -16);
    CHECK(p.x1 == 48);
    CHECK(p.y1 == 32);
    CHECK(p.cells() == 64u * 48u);
    CHECK(p.missing.empty());

    // The ore, eight own tiles turned as the cells were, solid, on the blocks.
    const std::vector<u8> ore = ground_tile(7);
    const u32 turns[8] = {0, kFlipH, kFlipV, kFlipH | kFlipV, kFlipD, kFlipD | kFlipH, kFlipD | kFlipV, kFlipD | kFlipH | kFlipV};
    std::vector<world::TileId> ore_ids;
    for (i32 i = 0; i < 8; ++i) {
        CAPTURE(i);
        const world::TileId id = p.at(1, 2 + i, 16);
        REQUIRE(id >= kFirstOwnTile);
        CHECK(p.at(0, 2 + i, 16) == 0);
        const OwnTile* t = p.tiles.find(id);
        REQUIRE(t);
        CHECK(t->solid);
        CHECK(t->layer == 1);
        CHECK(t->name.starts_with("Руда"));
        CHECK(picture_of(p.tiles, id) == turn(ore, turns[i]));
        ore_ids.push_back(id);
    }
    std::sort(ore_ids.begin(), ore_ids.end());
    CHECK(std::unique(ore_ids.begin(), ore_ids.end()) == ore_ids.end());
    // The same tile, the same own tile; at negative x too.
    CHECK(p.at(1, -12, 15) == p.at(1, 39, 15));
    CHECK(p.at(1, -9, 10) != 0); // the ledge of planks
    CHECK(p.tiles.find(p.at(1, -9, 10))->name == "Доски");
    // Walls: the sky (its see-through colour see-through), the cave wall with vines over it as one tile.
    const world::TileId cloud = p.at(0, -7, -3);
    REQUIRE(cloud != 0);
    CHECK_FALSE(p.tiles.find(cloud)->solid);
    const std::vector<u8> cp = picture_of(p.tiles, cloud);
    CHECK(cp[3] == 0); // the corner was #ff00ff
    const world::TileId vine_on_wall = p.at(0, 28, 10), wall = p.at(0, 27, 10);
    CHECK(p.tiles.find(vine_on_wall)->name == "Стена пещеры + Лоза");
    CHECK(p.tiles.find(wall)->name == "Стена пещеры");
    CHECK(picture_of(p.tiles, vine_on_wall) != picture_of(p.tiles, wall));
    CHECK(p.tiles.find(p.at(0, 33, 10))->name == "Стена пещеры + Лоза ↔");
    // Flowers alone on the background (over nothing).
    CHECK(p.tiles.find(p.at(0, 2, 13))->name == "Цветок");
    CHECK(p.tiles.find(p.at(0, -4, 13))->name == "Мухомор");
    // The hidden layer's arrow is not there.
    CHECK(p.at(0, 10, 5) == 0);
    CHECK(p.at(1, 10, 5) == 0);
    CHECK(p.tiles_new == p.tiles.tiles.size());
    CHECK(p.tiles_updated == 0);
    // Every own tile is in the record by its stack.
    CHECK(p.record.tiles.size() == p.tiles.tiles.size());

    // The zone, its music, the spawn point.
    REQUIRE(p.areas.areas.size() == 1);
    const Area& cave = p.areas.areas[0];
    CHECK(cave.name == "Пещера");
    CHECK(cave.x0 == 25);
    CHECK(cave.y0 == 9);
    CHECK(cave.x1 == 37);
    CHECK(cave.y1 == 14);
    CHECK(cave.music == "пещера.wav");
    REQUIRE(p.music.size() == 1);
    CHECK(fs::equivalent(p.music[0].first, example_dir() / utf8_path("музыка/пещера.wav")));
    CHECK(p.zones_new == 1);
    CHECK(p.areas.spawn);
    CHECK(p.areas.spawn_x == 3);
    CHECK(p.areas.spawn_y == 14);
    CHECK(p.record.spawn == 1);

    // Pictures: the chest (twice, one picture), the sign and the sign mirrored, the lantern (from the template).
    REQUIRE(p.pictures.size() == 4);
    std::vector<std::string> pictures;
    for (const Picture& pic : p.pictures) pictures.push_back(pic.name + " " + std::to_string(pic.w) + "×" + std::to_string(pic.h));
    CHECK(pictures == std::vector<std::string>{"Сундук 16×16", "Табличка 16×16", "Табличка ↔ 16×16", "Фонарь 16×32"});
    for (const Picture& pic : p.pictures) {
        CHECK_FALSE(pic.known);
        CHECK(pic.template_id.starts_with("tiled_уровень_предметы_"));
    }
    REQUIRE(p.objects.size() == 6);
    auto obj = [&](u32 id) -> const PlannedObject& {
        for (const PlannedObject& o : p.objects)
            if (o.tiled_id == id) return o;
        FAIL("no object " << id);
        return p.objects[0];
    };
    CHECK(obj(3).x == 34.5);
    CHECK(obj(3).y == 13.5);
    CHECK(obj(3).half_height == 0.5);
    CHECK(obj(7).x == 11);
    CHECK(obj(7).y == 13);
    CHECK(obj(7).half_height == 1);
    CHECK(obj(7).picture == obj(3).picture);
    CHECK(obj(8).x == -10.5);
    CHECK(obj(8).y == 13);
    CHECK(obj(8).name == "Фонарь у обрыва");
    CHECK(obj(6).picture == obj(8).picture);
    CHECK(obj(5).picture != obj(4).picture);
    for (const PlannedObject& o : p.objects) {
        CHECK(o.level_id != 0);
        CHECK_FALSE(o.known);
    }

    // What does not come over is told, with how many.
    u64 n = 0;
    CHECK(has_note(p.skipped, "эллипсы", &n));
    CHECK(n == 1);
    CHECK(has_note(p.skipped, "ломаные", &n));
    CHECK(n == 1);
    CHECK(has_note(p.skipped, "скрытые слои тайлов не переносятся: «Подсказки/Скрытое»"));
    CHECK_FALSE(has_note(p.skipped, "фон над блоками"));
}

TEST_CASE("Tiled: what is missing is told and its cells stay empty; the rest comes over") {
    const fs::path dir = copy_of_example("forge_test_tiled_missing");
    fs::remove(dir / utf8_path("наборы/декор.tsx"));
    fs::remove(dir / utf8_path("картинки/табличка.png"));
    fs::remove(dir / utf8_path("шаблоны/фонарь.tx"));
    fs::remove(dir / utf8_path("музыка/пещера.wav"));
    Map m;
    std::string why;
    REQUIRE_MESSAGE(read_map(dir / utf8_path("уровень.tmx"), m, &why), why);
    CHECK(m.tilesets[2].missing.find("нет файла наборы/декор.tsx") != std::string::npos);
    CHECK(object_with(m, 6)->template_missing.find("нет файла") != std::string::npos);
    const Plan p = plan_of(m);
    CAPTURE(notes_text(p.missing));
    CHECK(has_note(p.missing, "декор.tsx"));
    CHECK(has_note(p.missing, "табличка.png"));
    CHECK(has_note(p.missing, "фонарь.tx"));
    CHECK(has_note(p.missing, "пещера.wav"));
    // Flowers had only the decor: empty; the cave wall without its vines.
    CHECK(p.at(0, 2, 13) == 0);
    CHECK(p.tiles.find(p.at(0, 28, 10))->name == "Стена пещеры");
    CHECK(has_note(p.skipped, "оставшиеся пустыми"));
    // The chests and the lamp without a template are there; the signs and lanterns are not.
    std::vector<u32> ids;
    for (const PlannedObject& o : p.objects) ids.push_back(o.tiled_id);
    CHECK(ids == std::vector<u32>{3, 7});
    REQUIRE(p.areas.areas.size() == 1);
    CHECK(p.areas.areas[0].music.empty());
    CHECK(p.music.empty());
    fs::remove_all(dir);
}

TEST_CASE("Tiled: a second import of the same map keeps what each thing became; the author's own stay") {
    const fs::path dir = copy_of_example("forge_test_tiled_again");
    const fs::path tmx = dir / utf8_path("уровень.tmx");
    Map m;
    std::string why;
    REQUIRE(read_map(tmx, m, &why));
    const Plan first = plan_of(m);
    // The author's own zone and own tile, besides the import's.
    LevelAreas areas = first.areas;
    Area mine;
    mine.id = 77;
    mine.name = "Моя зона";
    mine.x0 = -5;
    mine.y0 = 0;
    mine.x1 = 0;
    mine.y1 = 5;
    areas.areas.push_back(mine);
    LevelTiles own = first.tiles;
    OwnTile mine_tile;
    mine_tile.id = static_cast<world::TileId>(kFirstOwnTile + 500);
    mine_tile.name = "Мой тайл";
    own.tiles.push_back(mine_tile);
    own.rgba.resize(own.rgba.size() + 16 * 16 * 4, 200);

    SUBCASE("the same map: the same ids, nothing new, nothing doubled") {
        const Plan again = plan_of(m, own, areas, first.record);
        CHECK(again.tiles_new == 0);
        CHECK(again.tiles_updated == 0);
        CHECK(again.tiles == own);
        CHECK(again.walls == first.walls);
        CHECK(again.blocks == first.blocks);
        CHECK(again.zones_new == 0);
        CHECK(again.zones_updated == 0);
        CHECK(again.zones_removed == 0);
        CHECK(again.areas == areas);
        REQUIRE(again.objects.size() == first.objects.size());
        for (usize i = 0; i < again.objects.size(); ++i) {
            CHECK(again.objects[i].known);
            CHECK(again.objects[i].level_id == first.objects[i].level_id);
        }
        for (const Picture& pic : again.pictures) CHECK(pic.known);
        CHECK(again.remove_objects.empty());
        CHECK(again.record.tiles == first.record.tiles);
        CHECK(again.record.objects == first.record.objects);
        CHECK(again.record.zones == first.record.zones);
    }
    SUBCASE("the map changed: what it no longer has goes, what is new comes, the ore that is gone keeps its own tile") {
        std::string text = read_text(tmx);
        // The sign goes, a new chest comes, the cave grows a cell to the left, the turned ore is grass now.
        const usize sign = text.find("  <object id=\"4\"");
        REQUIRE(sign != std::string::npos);
        text.erase(sign, text.find('\n', sign) + 1 - sign);
        const usize end = text.find(" </objectgroup>");
        text.insert(end, "  <object id=\"11\" name=\"Новый сундук\" gid=\"25\" x=\"64\" y=\"224\" width=\"16\" height=\"16\"/>\n");
        const std::string cave_was = "x=\"400\" y=\"144\" width=\"192\"";
        const usize cave = text.find(cave_was);
        REQUIRE(cave != std::string::npos);
        text.replace(cave, cave_was.size(), "x=\"384\" y=\"144\" width=\"208\"");
        for (const char* turned : {"2147483656", "1073741832", "3221225480", "536870920", "2684354568", "1610612744", "3758096392"}) {
            for (usize at; (at = text.find(std::string(",") + turned + ",")) != std::string::npos;) text.replace(at + 1, std::strlen(turned), "1");
        }
        write_text(tmx, text);
        Map changed;
        REQUIRE_MESSAGE(read_map(tmx, changed, &why), why);
        const Plan again = plan_of(changed, own, areas, first.record);
        CAPTURE(notes_text(again.skipped));
        // The sign's object goes; the new chest comes with a new id; the others keep theirs.
        REQUIRE(again.remove_objects.size() == 1);
        u64 sign_id = 0;
        for (const auto& [tid, lid] : first.record.objects)
            if (tid == 4) sign_id = lid;
        CHECK(again.remove_objects[0] == sign_id);
        for (const PlannedObject& o : again.objects) {
            CAPTURE(o.tiled_id);
            CHECK(o.known == (o.tiled_id != 11));
        }
        // The cave keeps its id, one cell wider; the author's zone is as it was.
        CHECK(again.zones_updated == 1);
        CHECK(again.zones_new == 0);
        REQUIRE(again.areas.find(first.areas.areas[0].id));
        CHECK(again.areas.find(first.areas.areas[0].id)->x0 == 24);
        REQUIRE(again.areas.find(77));
        CHECK(*again.areas.find(77) == mine);
        // The turned ore is gone from the map, its own tiles stay (cells the author painted with them keep them),
        // and stay in the record: the same ids if the map has them again.
        for (const auto& [key, id] : first.record.tiles)
            if (key.find("#7") != std::string::npos) {
                CAPTURE(key);
                CHECK(again.tiles.find(id));
                CHECK(std::find(again.record.tiles.begin(), again.record.tiles.end(), std::pair<std::string, world::TileId>{key, id}) !=
                      again.record.tiles.end());
            }
        CHECK(again.tiles.find(mine_tile.id));
        CHECK(again.tiles_new == 0);
        CHECK(again.at(1, 3, 16) == again.at(1, 39, 14)); // grass, the same own tile as before
    }
    fs::remove_all(dir);
}

TEST_CASE("tiled.json: written and read back the same; a broken one is told") {
    const fs::path dir = fs::temp_directory_path() / "forge_test_tiled_record";
    std::error_code ec;
    fs::remove_all(dir, ec);
    Record r;
    r.map = "уровень.tmx";
    r.x0 = -16;
    r.y0 = -16;
    r.x1 = 48;
    r.y1 = 32;
    r.tiles = {{"Блоки|наборы/Земля и камень.tsx#7dh", 300}, {"Фон|@Небо#0", 256}};
    r.objects = {{3, 0x9f2c41d07a5be318ull}, {11, 1}};
    r.zones = {{2, 0xfeull}};
    r.templates = {{"наборы/предметы.tsx#0", "tiled_уровень_предметы_0"}};
    r.spawn = 1;
    REQUIRE(save_record(dir, r));
    Record back;
    bool found = false;
    REQUIRE(load_record(dir, back, &found));
    CHECK(found);
    CHECK(back == r);
    write_text(dir / kRecordFile, "{\"map\": 5}");
    std::string why;
    CHECK_FALSE(load_record(dir, back, &found, &why));
    CHECK(why.find("map") != std::string::npos);
    CHECK(back == r); // unchanged
    REQUIRE(save_record(dir, {}));
    CHECK_FALSE(fs::exists(dir / kRecordFile));
    CHECK(load_record(dir, back, &found));
    CHECK_FALSE(found);
    CHECK(back.empty());
    fs::remove_all(dir, ec);
}
