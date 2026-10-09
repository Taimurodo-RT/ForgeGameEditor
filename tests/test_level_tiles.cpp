// The level's own tiles (tiles.json and tiles.png) and what is around it
// (world.json): kinds of tile with pictures of their own through undo, saves
// and reopening, every limit, bad files told and kept, a write error that
// loses nothing; and a level with nothing around, which has no generated
// tiles and nobody coming to live in it.

#include "forge/assets/image.h"
#include "forge/core/file.h"
#include "forge/core/jobs.h"
#include "forge/core/path.h"
#include "forge/editor/document.h"
#include "forge/editor/undo.h"
#include "forge/level/level.h"
#include "forge/level/own_tiles.h"
#include "forge/level/tile_edit.h"
#include "forge/world/generators.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace forge;
using namespace forge::level;
namespace fs = std::filesystem;
using world::ChunkCoord;
using world::Rect;
using world::TileId;

namespace {

struct PoolScope {
    PoolScope() { jobs::init(3); }
    ~PoolScope() { jobs::shutdown(); }
};

// Stone below y = 0 on layer 1, like a game's world.
struct StoneGenerator final : world::Generator {
    void generate(ChunkCoord coord, const world::ChunkTiles& out) const override {
        for (u32 l = 0; l < out.layer_count; ++l)
            for (i32 ly = 0; ly < world::kChunkSize; ++ly)
                for (i32 lx = 0; lx < world::kChunkSize; ++lx)
                    out.layer(l)[ly * world::kChunkSize + lx] = l == 1 && coord.y * world::kChunkSize + ly >= 0 ? 3 : 0;
    }
};

// Walls, blocks and liquids, as «Старая шахта»; peoples every chunk it is
// asked to.
struct TilesModule final : LevelModule {
    std::vector<std::string> layers{"Стены", "Блоки", "Жидкости"};
    std::vector<TileDef> palette{{"stone", "Камень", "Земля", "", 1, 3, ""}};
    u32 populated = 0;
    std::string title() const override { return "Тайлы"; }
    world::WorldDesc world_desc() const override {
        world::WorldDesc d;
        d.layer_count = 3;
        d.load_margin = 0;
        d.keep_extra = 0;
        return d;
    }
    std::shared_ptr<const world::Generator> generator() const override { return std::make_shared<StoneGenerator>(); }
    void setup_scene(scene::Scene& scene) override {
        scene.set_populator([this](ChunkCoord, scene::Scene&) { ++populated; });
    }
    const std::vector<std::string>& layer_names() const override { return layers; }
    i32 liquids_layer() const override { return 2; }
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

// n tiles of px, each picture its own: every pixel says which tile, where
// in it, and some see-through.
LevelTiles some_tiles(usize n, u32 px, u32 layer = 0) {
    LevelTiles t;
    t.px = px;
    for (usize i = 0; i < n; ++i) {
        t.tiles.push_back({static_cast<TileId>(kFirstOwnTile + i), "Тайл " + std::to_string(i + 1), layer, i % 2 == 1,
                           i == 0 ? "земля.tsx, тайл 0" : ""});
        for (u32 y = 0; y < px; ++y)
            for (u32 x = 0; x < px; ++x) {
                t.rgba.push_back(static_cast<u8>(i * 7 + 1));
                t.rgba.push_back(static_cast<u8>(x * 255 / px));
                t.rgba.push_back(static_cast<u8>(y * 255 / px));
                t.rgba.push_back(static_cast<u8>((x + y) % 3 == 0 ? 0 : 255));
            }
    }
    return t;
}

void load_view(Level& level, const Rect& view) {
    level.update(std::span<const Rect>(&view, 1));
    level.ensure_loaded(view);
}

} // namespace

TEST_CASE("own tiles: none is none; set, undone, saved and read back the same, pixel for pixel") {
    PoolScope pool;
    TilesModule module;
    const fs::path folder = temp_folder("forge_test_level_tiles");
    editor::Document doc;
    editor::UndoStack history(doc);
    const LevelTiles tiles = some_tiles(3, 16);
    {
        Level level(module);
        REQUIRE(level.open(folder));
        CHECK(level.own_tiles().empty());
        CHECK(level.own_tiles_error().empty());
        CHECK(level.save().ok);
        CHECK_FALSE(fs::exists(folder / kTilesFile)); // nothing changed: no files
        CHECK_FALSE(fs::exists(folder / kTilesPicture));
        const u64 v0 = level.own_tiles_version();
        // The kinds and the cells painted with them: one step, as an import makes them.
        load_view(level, {-8, -8, 8, 8});
        history.begin_group("Импорт");
        history.execute(std::make_unique<SetOwnTiles>(level, level.own_tiles(), tiles, "Тайлы уровня"));
        auto stroke = std::make_unique<TileStroke>(level, "Клетки");
        const Cell cells[] = {{0, -2}, {1, -2}};
        stroke->paint(0, cells, 256);
        stroke->paint(1, std::span<const Cell>(cells, 1), 257);
        history.execute(std::move(stroke));
        history.end_group();
        history.seal();
        CHECK(history.size() == 1);
        CHECK(level.own_tiles() == tiles);
        CHECK(level.own_tiles_version() > v0);
        CHECK(level.tile(0, 1, -2) == 256);
        history.undo();
        CHECK(level.own_tiles().empty());
        CHECK(level.tile(0, 1, -2) == 0);
        CHECK(level.tile(1, 0, -2) == 0);
        CHECK_FALSE(level.own_tiles_changed());
        history.redo();
        CHECK(level.own_tiles() == tiles);
        CHECK(level.tile(1, 0, -2) == 257);
        const Level::SaveReport r = level.save();
        CHECK(r.ok);
        CHECK(r.own_tiles);
        CHECK_FALSE(r.areas);
        CHECK_FALSE(level.own_tiles_changed());
        CHECK_FALSE(level.save().own_tiles); // saving again writes nothing more
    }
    const std::string text = read_text(folder / kTilesFile);
    CHECK(text.find("\"px\": 16") != std::string::npos);
    CHECK(text.find("\"id\": 257") != std::string::npos);
    CHECK(text.find("\"name\": \"Тайл 1\"") != std::string::npos);
    CHECK(text.find("\"from\": \"земля.tsx, тайл 0\"") != std::string::npos);
    CHECK(text == tiles_json(tiles));
    {
        // As the game reads it: a new level from the folder.
        Level level(module);
        REQUIRE(level.open(folder));
        CHECK(level.own_tiles_error().empty());
        CHECK(level.own_tiles() == tiles); // names, layers, solid, and every pixel
        load_view(level, {-8, -8, 8, 8});
        CHECK(level.tile(0, 0, -2) == 256);
        CHECK(level.tile(1, 0, -2) == 257);
        // None left: both files go.
        level.set_own_tiles({});
        CHECK(level.save().own_tiles);
    }
    CHECK_FALSE(fs::exists(folder / kTilesFile));
    CHECK_FALSE(fs::exists(folder / kTilesPicture));
    fs::remove_all(folder);
}

TEST_CASE("own tiles: pictures go 16 to a row; small and large ones; any ids in range") {
    const fs::path folder = temp_folder("forge_test_level_tiles_sizes");
    for (const auto& [n, px] : {std::pair<usize, u32>{1, 4}, {16, 16}, {17, 8}, {40, 32}, {33, 128}}) {
        CAPTURE(n);
        CAPTURE(px);
        LevelTiles t = some_tiles(n, px);
        // Ids need not follow one another.
        t.tiles.back().id = static_cast<TileId>(kFirstOwnTile + max_own_tiles(px) - 1);
        REQUIRE(tiles_problem(t, 3, 2).empty());
        REQUIRE(save_tiles(folder, t));
        std::vector<u8> bytes;
        REQUIRE(read_file(folder / kTilesPicture, bytes));
        assets::CookedTexture img;
        REQUIRE(assets::decode_image(bytes, img));
        CHECK(img.width == std::min<usize>(n, 16) * px);
        CHECK(img.height == (n + 15) / 16 * px);
        LevelTiles back;
        bool found = false;
        REQUIRE(load_tiles(folder, back, 3, 2, &found));
        CHECK(found);
        CHECK(back == t);
    }
    fs::remove_all(folder);
}

TEST_CASE("own tiles: limits") {
    CHECK(max_own_tiles(4) == kMaxOwnTiles);
    CHECK(max_own_tiles(16) == kMaxOwnTiles);
    CHECK(max_own_tiles(64) == 64 * 64 - 256);    // an atlas of 4096 px holds 64 × 64 cells
    CHECK(max_own_tiles(128) == 32 * 32 - 256);
    const LevelTiles good = some_tiles(3, 16);
    CHECK(tiles_problem(good, 3, 2).empty());
    CHECK(tiles_problem({}, 3, 2).empty());
    auto wrong = [&](auto change) {
        LevelTiles t = good;
        change(t);
        return tiles_problem(t, 3, 2);
    };
    CHECK_FALSE(wrong([](LevelTiles& t) { t.px = 3; }).empty());
    CHECK_FALSE(wrong([](LevelTiles& t) { t.px = 129; }).empty());
    CHECK_FALSE(wrong([](LevelTiles& t) { t.rgba.pop_back(); }).empty());
    CHECK_FALSE(wrong([](LevelTiles& t) { t.tiles.pop_back(); }).empty());
    CHECK_FALSE(wrong([](LevelTiles& t) { t.tiles[0].id = 255; }).empty());
    CHECK_FALSE(wrong([](LevelTiles& t) { t.tiles[0].id = static_cast<TileId>(kFirstOwnTile + kMaxOwnTiles); }).empty());
    CHECK(wrong([](LevelTiles& t) { t.tiles[2].id = 257; }).find("уже есть") != std::string::npos);
    CHECK(wrong([](LevelTiles& t) { t.tiles[1].name.clear(); }).find("нет имени") != std::string::npos);
    CHECK_FALSE(wrong([](LevelTiles& t) { t.tiles[1].name = "Тра\nва"; }).empty());
    std::string name;
    for (int i = 0; i < 60; ++i) name += "ш"; // 60 letters, 120 bytes
    CHECK(wrong([&](LevelTiles& t) { t.tiles[1].name = name; }).empty());
    CHECK_FALSE(wrong([&](LevelTiles& t) { t.tiles[1].name = name + "ш"; }).empty());
    CHECK_FALSE(wrong([](LevelTiles& t) { t.tiles[1].layer = 2; }).empty()); // liquids
    CHECK_FALSE(wrong([](LevelTiles& t) { t.tiles[1].layer = 3; }).empty());
    CHECK_FALSE(wrong([](LevelTiles& t) { t.tiles[1].from = std::string(201, 'a'); }).empty());
    // As many as fit, and not one more.
    LevelTiles big = some_tiles(max_own_tiles(128), 128);
    CHECK(tiles_problem(big, 3, 2).empty());
    big.tiles.push_back({static_cast<TileId>(kFirstOwnTile + max_own_tiles(128)), "Лишний", 0, false, ""});
    big.rgba.resize(big.tiles.size() * 128 * 128 * 4);
    CHECK(tiles_problem(big, 3, 2).find("больше") != std::string::npos);
    // A level takes none with a problem.
    PoolScope pool;
    TilesModule module;
    Level level(module);
    REQUIRE(level.open({}));
    LevelTiles t = good;
    t.tiles[0].layer = 2;
    CHECK_FALSE(level.set_own_tiles(t));
    CHECK(level.own_tiles().empty());
    CHECK(level.set_own_tiles(good));
    CHECK(level.own_tiles() == good);
}

TEST_CASE("tiles.json: bad files are told and kept, their cells keep their values; a new save keeps them aside") {
    PoolScope pool;
    TilesModule module;
    const fs::path folder = temp_folder("forge_test_level_tiles_bad");
    const LevelTiles good = some_tiles(2, 16);
    // A level whose cells were painted with its own tiles.
    {
        Level level(module);
        REQUIRE(level.open(folder));
        load_view(level, {-8, -8, 8, 8});
        REQUIRE(level.set_own_tiles(good));
        REQUIRE(level.set_tile(0, 2, -3, 257));
        REQUIRE(level.save().ok);
    }
    const std::string picture = read_text(folder / kTilesPicture);
    struct Bad {
        std::string json;
        bool picture = true; // a good tiles.png next to it
    };
    const std::string ok_tile = R"({"id": 256, "name": "А", "layer": 0})";
    const Bad bad[] = {
        {"не JSON"},
        {"[]"},
        {R"({"px": 16, "tiles": {}})"},
        {R"({"px": "16", "tiles": []})"},
        {R"({"px": 16, "tiles": [{"id": 256, "name": "А", "layer": 0}, {"id": 256, "name": "Б", "layer": 0}]})"},
        {R"({"px": 16, "tiles": [{"id": 255, "name": "А", "layer": 0}, {"id": 257, "name": "Б", "layer": 0}]})"},
        {R"({"px": 16, "tiles": [{"id": 256, "name": 5, "layer": 0}, {"id": 257, "name": "Б", "layer": 0}]})"},
        {R"({"px": 16, "tiles": [{"id": 256, "name": "А", "layer": 2}, {"id": 257, "name": "Б", "layer": 0}]})"},
        {R"({"px": 16, "tiles": [{"id": 256, "name": "А", "layer": 0, "solid": 1}, {"id": 257, "name": "Б", "layer": 0}]})"},
        {R"({"px": 8, "tiles": [{"id": 256, "name": "А", "layer": 0}, {"id": 257, "name": "Б", "layer": 0}]})"}, // picture too big
        {R"({"px": 16, "tiles": [)" + ok_tile + "]}"},                                                           // too big for one
        {R"({"px": 16, "tiles": [{"id": 256, "name": "А", "layer": 0}, {"id": 257, "name": "Б", "layer": 0}]})", false},
    };
    for (const Bad& b : bad) {
        CAPTURE(b.json);
        std::error_code ec;
        fs::remove(folder / kBrokenTilesFile, ec);
        fs::remove(folder / kBrokenTilesPicture, ec);
        write_text(folder / kTilesFile, b.json);
        if (b.picture) write_text(folder / kTilesPicture, picture);
        else fs::remove(folder / kTilesPicture, ec);
        Level level(module);
        REQUIRE(level.open(folder));
        CHECK(level.own_tiles().empty());
        CHECK(level.own_tiles_error().find(kTilesFile) != std::string::npos);
        load_view(level, {-8, -8, 8, 8});
        CHECK(level.tile(0, 2, -3) == 257); // the cell keeps its value
        CHECK(level.save().ok);
        CHECK(read_text(folder / kTilesFile) == b.json); // not overwritten while nothing changed
        REQUIRE(level.set_own_tiles(good));
        const Level::SaveReport r = level.save();
        CHECK(r.ok);
        CHECK(r.own_tiles);
        CHECK(level.own_tiles_error().empty());
        CHECK(read_text(folder / kBrokenTilesFile) == b.json); // kept aside, not lost
        CHECK(fs::exists(folder / kBrokenTilesPicture) == b.picture);
        LevelTiles back;
        CHECK(load_tiles(folder, back, 3, 2));
        CHECK(back == good);
    }
    // A picture that is not one.
    write_text(folder / kTilesPicture, "не картинка");
    LevelTiles back;
    std::string why;
    CHECK_FALSE(load_tiles(folder, back, 3, 2, nullptr, &why));
    CHECK(why.find(kTilesPicture) != std::string::npos);
    fs::remove_all(folder);
}

TEST_CASE("tiles.json: a write error is told, nothing is lost, the save is not called done") {
    PoolScope pool;
    TilesModule module;
    const fs::path folder = temp_folder("forge_test_level_tiles_write");
    Level level(module);
    REQUIRE(level.open(folder));
    const LevelTiles t = some_tiles(2, 16);
    for (const char* in_the_way : {kTilesPicture, kTilesFile}) {
        CAPTURE(in_the_way);
        fs::create_directories(folder / in_the_way / utf8_path("в пути")); // something in the way
        REQUIRE(level.set_own_tiles(t));
        const Level::SaveReport r = level.save();
        CHECK_FALSE(r.ok);
        CHECK_FALSE(r.own_tiles);
        CHECK(r.error.find(in_the_way) != std::string::npos);
        CHECK(level.own_tiles_changed());
        CHECK(level.own_tiles() == t);
        fs::remove_all(folder / in_the_way);
        CHECK(level.save().own_tiles);
        LevelTiles back;
        CHECK(load_tiles(folder, back, 3, 2));
        CHECK(back == t);
        REQUIRE(level.set_own_tiles({}));
        REQUIRE(level.save().ok);
    }
    fs::remove_all(folder);
}

namespace {

std::vector<u8> bytes_of(const fs::path& file) {
    std::vector<u8> bytes;
    CHECK(read_file(file, bytes));
    return bytes;
}

std::vector<std::string> names_in(const fs::path& folder) {
    std::vector<std::string> names;
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(folder, ec)) names.push_back(path_to_utf8(e.path().filename()));
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace

TEST_CASE("tiles.json and tiles.png go as a pair: when either cannot be written or moved, the pair that was there stays") {
    const fs::path folder = temp_folder("forge_test_level_tiles_pair");
    const LevelTiles before = some_tiles(2, 16), after = some_tiles(3, 32);
    REQUIRE(save_tiles(folder, before));
    const std::vector<u8> json = bytes_of(folder / kTilesFile), png = bytes_of(folder / kTilesPicture);
    // The first is how it was found: the second write (tiles.json) failed after tiles.png was already replaced.
    for (const char* in_the_way : {"tiles.json.tmp", "tiles.png.tmp", "tiles.json.old", "tiles.png.old"}) {
        CAPTURE(in_the_way);
        fs::create_directories(folder / in_the_way / utf8_path("в пути"));
        std::string why;
        CHECK_FALSE(save_tiles(folder, after, &why));
        CHECK_FALSE(why.empty());
        CHECK(bytes_of(folder / kTilesFile) == json);
        CHECK(bytes_of(folder / kTilesPicture) == png);
        std::vector<std::string> expected{in_the_way, kTilesFile, kTilesPicture};
        std::sort(expected.begin(), expected.end());
        CHECK(names_in(folder) == expected); // and nothing half written left
        LevelTiles back;
        REQUIRE(load_tiles(folder, back, 3, 2));
        CHECK(back == before);
        fs::remove_all(folder / in_the_way);
    }
    // Nothing in the way: the new pair, nothing left beside it.
    REQUIRE(save_tiles(folder, after));
    CHECK(names_in(folder) == std::vector<std::string>{kTilesFile, kTilesPicture});
    LevelTiles back;
    REQUIRE(load_tiles(folder, back, 3, 2));
    CHECK(back == after);
    fs::remove_all(folder);
}

TEST_CASE("tiles.json and tiles.png: none to save, tiles.png cannot be removed: told, tiles.json stays") {
    const fs::path folder = temp_folder("forge_test_level_tiles_remove");
    REQUIRE(save_tiles(folder, some_tiles(2, 16)));
    const std::vector<u8> json = bytes_of(folder / kTilesFile);
    fs::remove(folder / kTilesPicture);
    fs::create_directories(folder / kTilesPicture / utf8_path("в пути"));
    std::string why;
    CHECK_FALSE(save_tiles(folder, {}, &why));
    CHECK(why.find(kTilesPicture) != std::string::npos);
    CHECK(bytes_of(folder / kTilesFile) == json);
    CHECK(names_in(folder) == std::vector<std::string>{kTilesFile, kTilesPicture});
    // Once it can be, both go.
    fs::remove_all(folder / kTilesPicture);
    REQUIRE(save_tiles(folder, some_tiles(2, 16)));
    CHECK(save_tiles(folder, {}));
    CHECK(names_in(folder).empty());
    fs::remove_all(folder);
}

TEST_CASE("world.json: nothing around: no generated tiles, nobody comes to live; without it the game's world") {
    PoolScope pool;
    const fs::path folder = temp_folder("forge_test_level_world");
    // The empty generator: nothing on any layer.
    {
        std::vector<TileId> tiles(3 * world::kChunkTiles, 7);
        world::EmptyGenerator().generate({3, -2}, {tiles.data(), 3});
        CHECK(std::count(tiles.begin(), tiles.end(), TileId{0}) == static_cast<std::ptrdiff_t>(tiles.size()));
    }
    const Rect view{-80, -80, 80, 80};
    {
        // No world.json: the game's world around, peopled as always.
        TilesModule module;
        Level level(module);
        REQUIRE(level.open(folder));
        CHECK_FALSE(level.around().empty_around);
        load_view(level, view);
        CHECK(level.tile(1, 5, 10) == 3);
        CHECK(module.populated > 0);
        REQUIRE(level.set_tile(1, 5, 10, 0)); // a chunk of the level's own
        REQUIRE(level.save().ok);
    }
    LevelWorld w;
    w.empty_around = true;
    REQUIRE(save_world(folder, w));
    CHECK(read_text(folder / kWorldFile).find("\"around\": \"empty\"") != std::string::npos);
    {
        TilesModule module;
        Level level(module);
        REQUIRE(level.open(folder));
        CHECK(level.around().empty_around);
        CHECK(level.around_error().empty());
        load_view(level, view);
        CHECK(module.populated == 0);
        CHECK(level.tile(1, 5, 9) == 3);  // the level's own chunk is as saved
        CHECK(level.tile(1, 5, 10) == 0);
        CHECK(level.tile(1, 5, 70) == 0); // around it, nothing
        CHECK(level.tile(1, -70, 70) == 0);
    }
    // The game's world again: no file.
    REQUIRE(save_world(folder, {}));
    CHECK_FALSE(fs::exists(folder / kWorldFile));
    // Bad files: told, the game's world around, the file kept.
    for (const char* text : {"не JSON", "[]", R"({"around": "пусто"})", R"({"around": true})", R"({})"}) {
        CAPTURE(text);
        write_text(folder / kWorldFile, text);
        TilesModule module;
        Level level(module);
        REQUIRE(level.open(folder));
        CHECK_FALSE(level.around().empty_around);
        CHECK(level.around_error().find(kWorldFile) != std::string::npos);
        CHECK(level.save().ok);
        CHECK(read_text(folder / kWorldFile) == text);
    }
    write_text(folder / kWorldFile, R"({"around": "game"})");
    LevelWorld back{true};
    bool found = false;
    CHECK(load_world(folder, back, &found));
    CHECK(found);
    CHECK_FALSE(back.empty_around);
    fs::remove_all(folder);
}
