// An import of a Tiled map put into a level and a game: the example map of
// games/examples/tiled (re-saved by Tiled 1.8.2) into a level with the
// game's world around it, a villager in it and the author's own cell, object
// and zone. One step of the history, undone and redone; saved, undone and
// saved again, opened again; imported again unchanged (nothing new) and
// changed (what the author moved goes back, what the map lost goes, nothing
// doubled, the author's things stay); the game's files written all or
// nothing; a map past the edge of the world refused.

#include "forge/assets/image.h"
#include "forge/core/file.h"
#include "forge/core/jobs.h"
#include "forge/core/path.h"
#include "forge/editor/document.h"
#include "forge/editor/undo.h"
#include "forge/level/areas.h"
#include "forge/level/level.h"
#include "forge/level/object_edit.h"
#include "forge/level/tile_edit.h"
#include "forge/level/tiled.h"
#include "forge/level/tiled_apply.h"
#include "forge/level/tiled_import.h"
#include "forge/objects/library.h"
#include "forge/sim/bodies.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace forge;
using namespace forge::world;
using namespace forge::level;
namespace fs = std::filesystem;
namespace tl = forge::level::tiled;

namespace {

struct PoolScope {
    PoolScope() { jobs::init(3); }
    ~PoolScope() { jobs::shutdown(); }
};

// Stone from y 20 down on the blocks; water in the two rows over it from x -50 to 59 (a lake in the map's place).
struct HillsGenerator final : Generator {
    void generate(ChunkCoord coord, const ChunkTiles& out) const override {
        for (i32 ly = 0; ly < kChunkSize; ++ly)
            for (i32 lx = 0; lx < kChunkSize; ++lx) {
                const i32 x = coord.x * kChunkSize + lx, y = coord.y * kChunkSize + ly;
                out.layer(1)[ly * kChunkSize + lx] = y >= 20 ? 3 : 0;
                out.layer(2)[ly * kChunkSize + lx] = (y == 18 || y == 19) && x >= -50 && x < 60 ? 5 : 0;
            }
    }
};

const char* kKinds = R"({
  "blocks": [
    {"id": "still", "name": "Неподвижный",
     "components": {"Body": {"half_w": 0.5, "half_h": 0.5, "gravity": 0, "collide": false}},
     "props": [{"id": "half_height", "name": "Полвысоты", "bind": "Body.half_h", "min": 0.25, "max": 16}]}
  ],
  "kinds": [
    {"id": "picture", "name": "Картинка", "group": "Разное", "foot": 0.5, "blocks": ["still"],
     "presets": [{"name": "Картинка"}]}
  ]
})";

// The game: three layers, a villager in the middle of every chunk of the row y 0…63 (a Body, no template).
struct GameModule final : LevelModule {
    std::vector<std::string> layers{"Фон", "Блоки", "Жидкости"};
    std::vector<TileDef> palette{{"stone", "Камень", "Земля", "", 1, 3, ""}};
    objects::Library* lib = nullptr;
    u32 populated = 0;
    std::string title() const override { return "Игра"; }
    WorldDesc world_desc() const override {
        WorldDesc d;
        d.layer_count = 3;
        d.load_margin = 0;
        d.keep_extra = 0;
        d.bounds = {-8, -8, 8, 8}; // tiles -512…511
        return d;
    }
    std::shared_ptr<const Generator> generator() const override { return std::make_shared<HillsGenerator>(); }
    void setup_scene(scene::Scene& scene) override {
        scene.register_component<sim::Body>();
        lib->attach(scene);
        scene.set_populator([this](ChunkCoord c, scene::Scene& s) {
            ++populated;
            if (c.y != 0) return;
            flecs::entity e = s.spawn(scene::Position::at_tile(c.x * kChunkSize + 32.5, 19.5));
            if (e.is_valid()) e.set<sim::Body>({});
        });
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
    objects::Library* library() override { return lib; }
};

fs::path source_dir() { return utf8_path(FORGE_SOURCE_DIR); }
fs::path example_dir() { return source_dir() / "games" / "examples" / "tiled" / utf8_path("Карта Tiled"); }

std::string read_text(const fs::path& file) {
    std::vector<u8> bytes;
    if (!read_file(file, bytes)) return {};
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void write_text(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    REQUIRE(write_file_atomic(file, {reinterpret_cast<const u8*>(text.data()), text.size()}));
}

// Every file under a folder with its bytes.
std::map<std::string, std::vector<u8>> files_of(const fs::path& dir) {
    std::map<std::string, std::vector<u8>> out;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
        if (!e.is_regular_file()) continue;
        std::vector<u8> bytes;
        read_file(e.path(), bytes);
        out[path_to_utf8(fs::relative(e.path(), dir))] = std::move(bytes);
    }
    return out;
}

const Rect kView{-40, -40, 40, 40};

void load_view(Level& level) {
    level.update(std::span<const Rect>(&kView, 1));
    level.ensure_loaded(kView);
}

// The game's own objects loaded now: bodies without a LevelId.
u32 villagers(Level& level) {
    u32 n = 0;
    level.scene().ecs().each([&](flecs::entity e, const sim::Body&) {
        if (!e.has<LevelId>()) ++n;
    });
    return n;
}

u32 objects_of(Level& level, u64 key) {
    u32 n = 0;
    level.scene().ecs().each([&](flecs::entity, const objects::ObjectRef& r) {
        if (r.key == key) ++n;
    });
    return n;
}

// A game folder (kinds, templates, pictures, sounds) and a level folder, and a copy of the example map.
struct Project {
    fs::path root, game, level, map_dir, map;
    objects::Library lib;
    GameModule module;
    explicit Project(const char* name) {
        root = fs::temp_directory_path() / name;
        std::error_code ec;
        fs::remove_all(root, ec);
        game = root / "game";
        level = root / utf8_path("уровень");
        map_dir = root / utf8_path("Карта Tiled");
        map = map_dir / utf8_path("уровень.tmx");
        fs::create_directories(game / "objects");
        fs::create_directories(level);
        write_text(game / "kinds.json", kKinds);
        fs::copy(example_dir(), map_dir, fs::copy_options::recursive, ec);
        REQUIRE_MESSAGE(!ec, ec.message());
        REQUIRE(lib.load(game / "kinds.json", game / "objects"));
        module.lib = &lib;
    }
    fs::path sounds() const { return game / "sounds"; }
    tl::Plan plan_for(Level& l, std::string* refusal = nullptr) {
        tl::Map m;
        std::string why;
        REQUIRE_MESSAGE(tl::read_map(map, m, &why), why);
        tl::Options o;
        o.template_taken = [this](const std::string& id) { return tl::template_taken(lib, id); };
        tl::Plan p;
        const bool ok = tl::plan(m, o, l.own_tiles(), l.areas(), l.tiled_record(), p, &why);
        if (refusal) *refusal = why;
        else REQUIRE_MESSAGE(ok, why);
        return p;
    }
};

const tl::PlannedObject* planned(const tl::Plan& p, u32 tiled_id) {
    for (const tl::PlannedObject& o : p.objects)
        if (o.tiled_id == tiled_id) return &o;
    return nullptr;
}

} // namespace

TEST_CASE("Tiled import: one step of the history; undone, redone, saved, opened again; again with no change") {
    PoolScope pool;
    Project pr("forge_test_tiled_apply");
    editor::Document doc;
    editor::UndoStack history(doc);
    auto level = std::make_unique<Level>(pr.module);
    REQUIRE(level->open(pr.level));
    load_view(*level);
    CHECK(villagers(*level) == 2); // in chunks (-1, 0) and (0, 0)

    // The author's own: a cell in the map's place and one beside it (both in chunk 0, 0), a «Картинка» of their own
    // beside the map, a zone.
    auto stroke = std::make_unique<TileStroke>(*level, "Кисть");
    const Cell inside[] = {{10, 25}}, beside[] = {{50, 5}};
    stroke->paint(1, inside, 4);
    stroke->paint(1, beside, 4);
    history.execute(std::move(stroke));
    history.seal();
    const objects::KindDef* picture = pr.lib.kind(tl::kPictureKind);
    REQUIRE(picture);
    std::optional<objects::Template> flag = pr.lib.make(*picture, nullptr, "Мой флаг");
    REQUIRE(flag);
    REQUIRE(pr.lib.put(*flag));
    const objects::Template* mine = pr.lib.find(std::string_view(flag->id));
    REQUIRE(mine);
    flecs::entity f = pr.lib.spawn(level->scene(), *mine, 55.5, 10);
    REQUIRE(f.is_valid());
    const ObjectSnapshot flag_snap = snapshot(*level, f);
    f.destruct();
    history.execute(std::make_unique<ObjectsCommand>(*level, std::vector<ObjectSnapshot>{flag_snap}, true, "Поставить"));
    LevelAreas areas = level->areas();
    Area zone;
    zone.id = 77;
    zone.name = "Моя зона";
    zone.x0 = 50, zone.y0 = 0, zone.x1 = 60, zone.y1 = 5;
    areas.areas.push_back(zone);
    history.execute(std::make_unique<SetAreas>(*level, level->areas(), areas, "Зона"));
    history.seal();
    REQUIRE(level->save().ok);
    const LevelAreas areas_before = level->areas();
    const auto level_files = files_of(pr.level);
    const auto game_files = files_of(pr.game);
    const usize entries = history.size();

    tl::Plan plan = pr.plan_for(*level);
    REQUIRE(plan.objects.size() == 6);
    REQUIRE(plan.pictures.size() == 4);
    const tl::ApplyOptions options;

    SUBCASE("what the window shows changes nothing; cancelling leaves every file as it was") {
        const tl::Preview pv = tl::preview(*level, pr.lib, plan, options);
        CHECK(pv.refusal.empty());
        CHECK(pv.cells > 0);
        CHECK(pv.author_cells == 1); // (10, 25): the stone of the game under it was the author's cell 4
        CHECK(pv.around);
        CHECK(pv.game_objects == 2); // one in the map's place, one in a chunk nothing will be around
        CHECK(pv.objects_new == 6);
        CHECK(pv.objects_back.empty());
        CHECK(pv.objects_removed == 0);
        CHECK(pv.templates_new == 4);
        CHECK(pv.templates_updated == 0);
        CHECK(history.size() == entries);
        CHECK(level->tile(1, 10, 25) == 4);
        CHECK(level->own_tiles().empty());
        CHECK_FALSE(level->around().empty_around);
        CHECK(level->areas() == areas_before);
        CHECK(villagers(*level) == 2);
        CHECK(level->save().ok);
        CHECK(files_of(pr.level) == level_files);
        CHECK(files_of(pr.game) == game_files);
    }

    SUBCASE("applied: one step; undo and redo; saved, undone and saved, opened again") {
        tl::Resources made;
        std::string why;
        REQUIRE_MESSAGE(tl::write_resources(plan, pr.lib, pr.sounds(), made, &why), why);
        CHECK(made.pictures.size() == 4);
        CHECK(made.templates.size() == 4);
        REQUIRE(made.sounds.size() == 1);
        CHECK(made.sounds[0] == "пещера.wav");
        CHECK(read_text(pr.sounds() / utf8_path("пещера.wav")) == read_text(pr.map_dir / utf8_path("музыка/пещера.wav")));
        for (const std::string& name : made.pictures) {
            CAPTURE(name);
            CHECK(name.rfind("tiled_уровень_", 0) == 0);
            CHECK(fs::is_regular_file(pr.game / "pictures" / utf8_path(name)));
        }
        // Nothing left aside.
        for (const auto& e : fs::directory_iterator(pr.game / "pictures")) CHECK(e.is_regular_file());
        for (const tl::Picture& p : plan.pictures) {
            const objects::Template* t = pr.lib.find(std::string_view(p.template_id));
            REQUIRE(t);
            CHECK(t->kind == tl::kPictureKind);
            CHECK(t->name.find(p.name.substr(0, 4)) == 0);
            CHECK(fs::is_regular_file(pr.lib.picture_file(*t)));
        }
        const objects::Template* lamp = nullptr;
        for (const tl::Picture& p : plan.pictures)
            if (p.name == "Фонарь") lamp = pr.lib.find(std::string_view(p.template_id));
        REQUIRE(lamp);
        CHECK(*lamp->value("half_height") == "1"); // a picture 16 × 32 on a map of 16: two cells high

        REQUIRE_MESSAGE(tl::apply(*level, history, pr.lib, plan, options, &why), why);
        REQUIRE(history.size() == entries + 1);
        CHECK(history.label_at(entries) == "Импорт карты Tiled «уровень.tmx»");
        CHECK(history.undo_label() == "Импорт карты Tiled «уровень.tmx»");

        auto check_imported = [&](Level& l, bool loaded_villagers = true) {
            load_view(l);
            l.load_objects();
            for (i32 i = 0; i < 8; ++i) CHECK(l.tile(1, 2 + i, 16) == plan.at(1, 2 + i, 16));
            CHECK(l.tile(1, 0, 14) == plan.at(1, 0, 14));
            CHECK(l.tile(0, 9, 11) == plan.at(0, 9, 11)); // the window over the brick wall
            CHECK(l.tile(1, 10, 25) == 0);   // the author's cell in the map's place: the map's (nothing)
            CHECK(l.tile(1, 20, 30) == 0);   // the game's stone in the map's place too
            CHECK(l.tile(2, 0, 18) == 0);    // and its water
            CHECK(l.tile(1, 50, 5) == 4);    // the author's cell beside the map stays
            CHECK(l.tile(1, 50, 25) == 3);   // in that chunk (changed), the game's stone stays
            CHECK(l.tile(1, -20, 25) == 0);  // in a chunk nobody changed, nothing around
            CHECK(l.tile(2, -20, 18) == 0);
            CHECK(l.own_tiles() == plan.tiles);
            CHECK(l.around().empty_around);
            CHECK(l.areas() == plan.areas);
            CHECK(l.areas().find(77) != nullptr); // the author's zone stays
            CHECK(l.areas().spawn);
            CHECK(l.areas().spawn_x == 3);
            CHECK(l.areas().spawn_y == 14);
            CHECK(l.tiled_record() == plan.record);
            for (const tl::PlannedObject& po : plan.objects) {
                CAPTURE(po.name);
                const flecs::entity e = l.find(po.level_id);
                REQUIRE(e.is_valid());
                CHECK(e.get<scene::Position>().tile_x() == doctest::Approx(po.x));
                CHECK(e.get<scene::Position>().tile_y() == doctest::Approx(po.y));
                CHECK(e.get<sim::Body>().half_h == doctest::Approx(po.half_height));
                CHECK(e.get<objects::ObjectRef>().key == pr.lib.find(std::string_view(plan.pictures[po.picture].template_id))->key);
            }
            const tl::PlannedObject* big = planned(plan, 7);
            REQUIRE(big);
            CHECK(l.find(big->level_id).get<sim::Body>().half_h == doctest::Approx(1)); // 32 × 32: its own size
            CHECK(l.find(flag_snap.id).is_valid());
            if (loaded_villagers) CHECK(villagers(l) == 0);
        };
        check_imported(*level);
        // Nobody comes to live in a chunk first seen now.
        const Rect far{-300, -40, -240, 40};
        level->ensure_loaded(far);
        CHECK(level->tile(1, -280, 30) == 0);
        CHECK(villagers(*level) == 0);

        auto check_before = [&](Level& l, u32 people) {
            load_view(l);
            l.load_objects();
            CHECK(l.tile(1, 10, 25) == 4);
            CHECK(l.tile(1, 20, 30) == 3);
            CHECK(l.tile(2, 0, 18) == 5);
            CHECK(l.tile(1, -20, 25) == 3);
            CHECK(l.tile(1, 2, 16) == 0);
            CHECK(l.own_tiles().empty());
            CHECK_FALSE(l.around().empty_around);
            CHECK(l.areas() == areas_before);
            CHECK(l.tiled_record().empty());
            for (const tl::PlannedObject& po : plan.objects) CHECK_FALSE(l.find(po.level_id).is_valid());
            CHECK(l.find(flag_snap.id).is_valid());
            CHECK(villagers(l) == people);
            // Chunks nobody had changed are the game's again, not the level's.
            CHECK_FALSE(l.world().edited({-1, 0}));
            CHECK_FALSE(l.world().edited({-1, -1}));
            CHECK(l.world().edited({0, 0}));
        };
        REQUIRE(history.undo());
        check_before(*level, 2);
        // The far chunks first seen with nothing around are peopled when they are seen with the game's world.
        level->ensure_loaded(far);
        CHECK(level->tile(1, -280, 30) == 3);
        CHECK(villagers(*level) == 4);
        // The templates and their pictures stay (the window said so); the level does not show them.
        for (const tl::Picture& p : plan.pictures) CHECK(pr.lib.find(std::string_view(p.template_id)));
        REQUIRE(history.redo());
        check_imported(*level);
        REQUIRE(history.undo());
        REQUIRE(history.redo());
        check_imported(*level);

        Level::SaveReport r = level->save();
        REQUIRE_MESSAGE(r.ok, r.error);
        CHECK(r.own_tiles);
        CHECK(r.around);
        CHECK(r.areas);
        CHECK(r.tiled);
        for (const char* name : {kTilesFile, kTilesPicture, kWorldFile, tl::kRecordFile, kAreasFile})
            CHECK(fs::is_regular_file(pr.level / name));

        // Undone and saved: the level's files as before the import (the chunks nobody had changed leave the folder).
        REQUIRE(history.undo());
        r = level->save();
        REQUIRE_MESSAGE(r.ok, r.error);
        for (const char* name : {kTilesFile, kTilesPicture, kWorldFile, tl::kRecordFile}) CHECK_FALSE(fs::exists(pr.level / name));
        level = std::make_unique<Level>(pr.module);
        REQUIRE(level->open(pr.level));
        check_before(*level, 4);

        // Done again in the level as opened, saved, opened again: the same.
        history.clear();
        plan = pr.plan_for(*level);
        REQUIRE_MESSAGE(tl::write_resources(plan, pr.lib, pr.sounds(), made, &why), why);
        CHECK(made.pictures.empty()); // the same pictures are there
        CHECK(made.templates.empty());
        CHECK(made.sounds.empty());
        REQUIRE_MESSAGE(tl::apply(*level, history, pr.lib, plan, options, &why), why);
        r = level->save();
        REQUIRE_MESSAGE(r.ok, r.error);
        level = std::make_unique<Level>(pr.module);
        REQUIRE(level->open(pr.level));
        CHECK(level->tiled_record_error().empty());
        check_imported(*level);

        // The same map again: nothing new, nothing doubled, no step in the history.
        const usize steps = history.size();
        tl::Plan again = pr.plan_for(*level);
        CHECK(again.tiles_new == 0);
        CHECK(again.tiles_updated == 0);
        const tl::Preview pv = tl::preview(*level, pr.lib, again, options);
        CHECK(pv.cells == 0);
        CHECK_FALSE(pv.around);
        CHECK(pv.objects_new == 0);
        CHECK(pv.objects_same == 6);
        CHECK(pv.objects_back.empty());
        CHECK(pv.templates_new == 0);
        CHECK(pv.templates_updated == 0);
        CHECK(pv.nothing());
        REQUIRE_MESSAGE(tl::write_resources(again, pr.lib, pr.sounds(), made, &why), why);
        CHECK(made.pictures.empty());
        CHECK(made.templates.empty());
        REQUIRE_MESSAGE(tl::apply(*level, history, pr.lib, again, options, &why), why);
        CHECK(history.size() == steps);
        for (const tl::Picture& p : plan.pictures) {
            const u64 key = pr.lib.find(std::string_view(p.template_id))->key;
            u32 want = 0;
            for (const tl::PlannedObject& po : plan.objects) want += plan.pictures[po.picture].key == p.key;
            CHECK(objects_of(*level, key) == want);
        }
    }
    fs::remove_all(pr.root);
}

TEST_CASE("Tiled import again after the author's changes and the map's: back as in the map, nothing doubled, the author's own stay") {
    PoolScope pool;
    Project pr("forge_test_tiled_again_apply");
    editor::Document doc;
    editor::UndoStack history(doc);
    Level level(pr.module);
    REQUIRE(level.open(pr.level));
    load_view(level);
    tl::Plan first = pr.plan_for(level);
    tl::Resources made;
    std::string why;
    REQUIRE_MESSAGE(tl::write_resources(first, pr.lib, pr.sounds(), made, &why), why);
    REQUIRE_MESSAGE(tl::apply(level, history, pr.lib, first, {}, &why), why);
    const tl::PlannedObject& chest = *planned(first, 3);
    const tl::PlannedObject& sign = *planned(first, 4);
    const tl::PlannedObject& big = *planned(first, 7);

    // The author moves the chest, deletes the sign, paints with an imported tile beside the map, adds a zone.
    history.execute(std::make_unique<MoveObjects>(level, std::vector<MoveObjects::Move>{{chest.level_id, chest.x, chest.y, chest.x + 3, chest.y}},
                                                  "Двигать"));
    history.seal();
    history.execute(std::make_unique<ObjectsCommand>(level, std::vector<ObjectSnapshot>{snapshot(level, level.find(sign.level_id))}, false,
                                                     "Удалить"));
    history.seal();
    const world::TileId ore = first.at(1, 2, 16);
    auto stroke = std::make_unique<TileStroke>(level, "Кисть");
    const Cell beside[] = {{60, 5}};
    stroke->paint(1, beside, ore);
    history.execute(std::move(stroke));
    history.seal();
    LevelAreas areas = level.areas();
    Area zone;
    zone.id = 99;
    zone.name = "Своя";
    zone.x0 = 50, zone.y0 = 0, zone.x1 = 60, zone.y1 = 5;
    areas.areas.push_back(zone);
    history.execute(std::make_unique<SetAreas>(level, level.areas(), areas, "Зона"));
    history.seal();

    // The map changes: the big chest goes, a new sign comes, the ore's tile is drawn anew, the chest's picture too.
    std::string text = read_text(pr.map);
    const usize at = text.find("  <object id=\"7\"");
    REQUIRE(at != std::string::npos);
    text.erase(at, text.find('\n', at) + 1 - at);
    const usize objects_end = text.find(" </objectgroup>");
    REQUIRE(objects_end != std::string::npos);
    const usize sign_line = text.find("  <object id=\"4\"");
    std::string extra = text.substr(sign_line, text.find('\n', sign_line) + 1 - sign_line);
    extra.replace(extra.find("id=\"4\""), 6, "id=\"40\"");
    extra.replace(extra.find("x=\"288\""), 7, "x=\"608\"");
    text.insert(objects_end, extra);
    write_text(pr.map, text);
    {
        // A dot on the chest's picture.
        std::vector<u8> bytes;
        REQUIRE(read_file(pr.map_dir / utf8_path("картинки/сундук.png"), bytes));
        assets::CookedTexture img;
        REQUIRE(assets::decode_image(bytes, img));
        img.rgba8[(8 * img.width + 8) * 4 + 0] = 1;
        img.rgba8[(8 * img.width + 8) * 4 + 3] = 255;
        REQUIRE(assets::encode_image(img, ".png", bytes));
        REQUIRE(write_file_atomic(pr.map_dir / utf8_path("картинки/сундук.png"), bytes));
    }
    const objects::Template* chest_t = pr.lib.find(std::string_view(first.pictures[chest.picture].template_id));
    REQUIRE(chest_t);
    const std::string old_picture = chest_t->picture;

    tl::Plan again = pr.plan_for(level);
    const tl::Preview pv = tl::preview(level, pr.lib, again, {});
    CHECK(pv.objects_new == 1);
    CHECK(pv.objects_removed == 1);
    CHECK(pv.templates_new == 0);
    CHECK(pv.templates_updated == 1);
    const std::string back = [&] {
        std::string s;
        for (const std::string& b : pv.objects_back) s += b + "\n";
        return s;
    }();
    CAPTURE(back);
    CHECK(back.find("Сундук (3): сдвинут") != std::string::npos);
    CHECK(back.find("Табличка (4): удалён, вернётся") != std::string::npos);
    CHECK(pv.objects_same == 3); // the mirrored sign and the two lanterns

    REQUIRE_MESSAGE(tl::write_resources(again, pr.lib, pr.sounds(), made, &why), why);
    CHECK(made.pictures.size() == 1);
    CHECK(made.templates.size() == 1);
    chest_t = pr.lib.find(std::string_view(first.pictures[chest.picture].template_id));
    CHECK(chest_t->picture != old_picture);
    CHECK_FALSE(fs::exists(pr.game / "pictures" / utf8_path(old_picture))); // no template shows it now
    CHECK(fs::is_regular_file(pr.game / "pictures" / utf8_path(chest_t->picture)));

    const usize steps = history.size();
    REQUIRE_MESSAGE(tl::apply(level, history, pr.lib, again, {}, &why), why);
    CHECK(history.size() == steps + 1);
    level.load_objects();
    CHECK(level.find(chest.level_id).get<scene::Position>().tile_x() == doctest::Approx(chest.x));
    CHECK(level.find(sign.level_id).is_valid());
    CHECK_FALSE(level.find(big.level_id).is_valid());
    const tl::PlannedObject* fresh = planned(again, 40);
    REQUIRE(fresh);
    CHECK_FALSE(fresh->known);
    CHECK(level.find(fresh->level_id).is_valid());
    CHECK(objects_of(level, chest_t->key) == 1); // the big chest was the other one
    CHECK(level.tile(1, 60, 5) == ore);          // the author's cell of an imported tile
    CHECK(level.own_tiles().find(ore) != nullptr);
    CHECK(level.areas().find(99) != nullptr);
    CHECK(level.areas().find(99)->name == "Своя");
    // One Ctrl+Z: the author's level as it was before this import.
    REQUIRE(history.undo());
    level.load_objects();
    CHECK(level.find(chest.level_id).get<scene::Position>().tile_x() == doctest::Approx(chest.x + 3));
    CHECK_FALSE(level.find(sign.level_id).is_valid());
    CHECK(level.find(big.level_id).is_valid());
    CHECK_FALSE(level.find(fresh->level_id).is_valid());
    CHECK(level.tiled_record() == first.record);
    REQUIRE(history.redo());
    fs::remove_all(pr.root);
}

TEST_CASE("Tiled import: the game's files are written all or nothing; a map past the world's edge is refused") {
    PoolScope pool;
    Project pr("forge_test_tiled_fail");
    editor::Document doc;
    editor::UndoStack history(doc);
    Level level(pr.module);
    REQUIRE(level.open(pr.level));
    load_view(level);
    const tl::Plan plan = pr.plan_for(level);
    const auto game_files = files_of(pr.game);
    const usize templates = pr.lib.templates().size();
    auto unchanged = [&] {
        CHECK(files_of(pr.game) == game_files);
        CHECK(pr.lib.templates().size() == templates);
        CHECK(history.size() == 0);
        CHECK(level.own_tiles().empty());
    };
    std::string why;
    tl::Resources made;

    SUBCASE("a picture cannot be moved in: the ones moved go back out") {
        // The last picture's place is taken by a folder.
        tl::Plan p = plan;
        tl::Resources first;
        // Its name, as write_resources makes it: found by a run into a scratch library.
        {
            Project scratch("forge_test_tiled_fail_names");
            tl::Plan q = scratch.plan_for(level);
            REQUIRE(tl::write_resources(q, scratch.lib, scratch.sounds(), first, &why));
            fs::remove_all(scratch.root);
        }
        REQUIRE(first.pictures.size() == 4);
        fs::create_directories(pr.game / "pictures" / utf8_path(first.pictures.back()) / "занято");
        const auto files = files_of(pr.game);
        CHECK_FALSE(tl::write_resources(p, pr.lib, pr.sounds(), made, &why));
        CAPTURE(why);
        CHECK(why.find("не перенеслась") != std::string::npos);
        CHECK(why.find("ничего не изменилось") != std::string::npos);
        CHECK(files_of(pr.game) == files);
        CHECK(pr.lib.templates().size() == templates);
        for (const auto& e : fs::directory_iterator(pr.game / "pictures"))
            CHECK(path_to_utf8(e.path().filename()).rfind(".tiled-import-", 0) != 0);
    }
    SUBCASE("the music cannot be written: the pictures go") {
        write_text(pr.sounds(), "не папка");
        const auto files = files_of(pr.game);
        tl::Plan p = plan;
        CHECK_FALSE(tl::write_resources(p, pr.lib, pr.sounds(), made, &why));
        CAPTURE(why);
        CHECK(why.find("пещера.wav") != std::string::npos);
        CHECK(files_of(pr.game) == files);
        CHECK(pr.lib.templates().size() == templates);
    }
    SUBCASE("a template cannot be written: the pictures and the music go, no template is made") {
        fs::remove_all(pr.game / "objects");
        write_text(pr.game / "objects", "не папка");
        const auto files = files_of(pr.game);
        tl::Plan p = plan;
        CHECK_FALSE(tl::write_resources(p, pr.lib, pr.sounds(), made, &why));
        CAPTURE(why);
        CHECK(why.find("шаблон") != std::string::npos);
        CHECK(files_of(pr.game) == files);
        CHECK(pr.lib.templates().size() == templates);
        CHECK_FALSE(fs::exists(pr.sounds() / utf8_path("пещера.wav")));
    }
    SUBCASE("a sound of that name that is another sound: the music gets its own name, the zone takes it") {
        write_text(pr.sounds() / utf8_path("пещера.wav"), "другой звук");
        tl::Plan p = plan;
        REQUIRE_MESSAGE(tl::write_resources(p, pr.lib, pr.sounds(), made, &why), why);
        REQUIRE(made.sounds.size() == 1);
        CHECK(made.sounds[0] == "пещера (2).wav");
        CHECK(read_text(pr.sounds() / utf8_path("пещера.wav")) == "другой звук");
        bool zone = false;
        for (const Area& a : p.areas.areas) zone |= a.music == "пещера (2).wav";
        CHECK(zone);
    }
    SUBCASE("a map past the edge of the game's world: refused, nothing changes") {
        tl::Plan far = plan;
        far.x0 += 600;
        far.x1 += 600;
        const tl::Preview pv = tl::preview(level, pr.lib, far, {});
        CHECK(pv.refusal.find("край мира") != std::string::npos);
        CHECK_FALSE(tl::apply(level, history, pr.lib, far, {}, &why));
        CHECK(why.find("край мира") != std::string::npos);
        unchanged();
    }
    SUBCASE("without the templates in the game: refused, nothing changes") {
        CHECK_FALSE(tl::apply(level, history, pr.lib, plan, {}, &why));
        CHECK(why.find("нет шаблона") != std::string::npos);
        unchanged();
    }
    fs::remove_all(pr.root);
}
