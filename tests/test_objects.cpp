#include "forge/core/file.h"
#include "forge/core/jobs.h"
#include "forge/core/path.h"
#include "forge/objects/library.h"
#include "forge/scene/scene.h"
#include "forge/world/generators.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <memory>
#include <string>

// A pickup as a game would have it.
struct ObjTestPickup {
    forge::u8 what = 0;
    forge::u16 count = 1;
    bool spins = false;
    std::string ding; // a sound
};
struct ObjTestBody {
    forge::f32 half = 0.5f;
};
FORGE_REFLECT_DECLARE(ObjTestPickup)
FORGE_REFLECT_DECLARE(ObjTestBody)
FORGE_REFLECT(ObjTestPickup, 1) {
    t.field("what", &ObjTestPickup::what);
    t.field("count", &ObjTestPickup::count).range(1, 999);
    t.field("spins", &ObjTestPickup::spins);
    t.field("ding", &ObjTestPickup::ding);
}
FORGE_REFLECT(ObjTestBody, 1) { t.field("half", &ObjTestBody::half); }

using namespace forge;
using namespace forge::objects;
using namespace forge::scene;
using namespace forge::world;

namespace {

struct PoolScope {
    PoolScope() { jobs::init(3); }
    ~PoolScope() { jobs::shutdown(); }
};

const char* kKinds = R"({
  "kinds": [
    {
      "id": "pickup", "name": "Подбираемое", "group": "Предметы", "icon": "paid",
      "about": "Герой подбирает его, касаясь.", "foot": 0.3,
      "components": { "ObjTestBody": {"half": 0.3}, "ObjTestPickup": {"count": 5} },
      "props": [
        {"id": "what", "name": "Что это", "bind": "ObjTestPickup.what",
         "choices": [{"id": "coins", "name": "Монеты", "value": 1}, {"id": "gem", "name": "Камень", "value": 2}]},
        {"id": "count", "name": "Сколько", "bind": "ObjTestPickup.count"},
        {"id": "spins", "name": "Крутится", "bind": "ObjTestPickup.spins", "advanced": true},
        {"id": "ding", "name": "Звон", "bind": "ObjTestPickup.ding", "asset": "sound", "empty": "обычный"},
        {"id": "broken", "name": "Нет такого", "bind": "ObjTestPickup.nothing"}
      ],
      "presets": [ {"name": "Монетка", "genre": "Платформер", "values": {"what": "coins", "count": 1, "spins": true}} ]
    }
  ]
})";

std::filesystem::path temp_folder(const char* name) {
    const auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

void write_text(const std::filesystem::path& p, const std::string& text) {
    REQUIRE(write_file_atomic(p, std::span(reinterpret_cast<const u8*>(text.data()), text.size())));
}

void settle(World& w, Scene& s, const Rect& view) {
    for (int i = 0; i < 4; ++i) {
        s.update();
        w.update(view);
        w.finish_loading();
    }
    s.update();
}

WorldDesc tight_desc() {
    WorldDesc d;
    d.load_margin = 0;
    d.keep_extra = 0;
    return d;
}

const Rect kHome{0, 0, 128, 128};
const Rect kAway{100'000, 100'000, 100'064, 100'064};

struct Fixture {
    std::filesystem::path dir = temp_folder("forge_objects_test");
    Library lib;
    Fixture() {
        write_text(dir / "kinds.json", kKinds);
        write_text(dir / "objects" / "coins.object.json",
                   R"({"id": "coins", "name": "Монеты", "kind": "pickup", "values": {"what": "coins", "count": 10}})");
        REQUIRE(lib.load(dir / "kinds.json", dir / "objects"));
    }
};

} // namespace

TEST_CASE("kinds and templates load from files") {
    Fixture f;
    REQUIRE(f.lib.kinds().size() == 1);
    const KindDef& k = f.lib.kinds()[0];
    CHECK(k.name == "Подбираемое");
    CHECK(k.props.size() == 4); // the one bound to a missing field is dropped
    CHECK(k.prop("ding")->asset == "sound");
    CHECK(Library::display(*k.prop("ding"), "\"\"") == "обычный");
    CHECK(Library::display(*k.prop("ding"), "\"звон.ogg\"") == "звон.ogg");
    CHECK(k.prop("count")->has_range);
    CHECK(k.presets.size() == 1);
    const Template* coins = f.lib.find("coins");
    REQUIRE(coins);
    CHECK(coins->name == "Монеты");
    CHECK(f.lib.value(*coins, *k.prop("count")) == "10");
    CHECK(f.lib.value(*coins, *k.prop("what")) == "\"coins\"");
    CHECK(f.lib.value(*coins, *k.prop("spins")) == "false"); // the field's default
    CHECK(Library::display(*k.prop("what"), "\"coins\"") == "Монеты");
    CHECK(Library::parse(*k.prop("what"), "Камень") == std::optional<std::string>("\"gem\""));
    CHECK(Library::parse(*k.prop("count"), "5000") == std::optional<std::string>("999"));
    CHECK(Library::parse(*k.prop("spins"), "да") == std::optional<std::string>("true"));
    CHECK(!Library::parse(*k.prop("count"), "много"));
}

TEST_CASE("new templates are written to files and read back") {
    Fixture f;
    const KindDef& k = f.lib.kinds()[0];
    std::optional<Template> t = f.lib.make(k, &k.presets[0], "");
    REQUIRE(t);
    CHECK(t->name == "Монетка");
    REQUIRE(f.lib.put(*t));
    std::optional<Template> again = f.lib.make(k, &k.presets[0], "");
    CHECK(again->name == "Монетка 2");
    CHECK(again->file != t->file);
    std::optional<Template> read = read_template(t->file);
    REQUIRE(read);
    CHECK(read->id == t->id);
    CHECK(read->values == t->values);
    f.lib.reload_templates();
    CHECK(f.lib.templates().size() == 2);
    CHECK(f.lib.remove(t->key));
    CHECK(!std::filesystem::exists(t->file));
    f.lib.reload_templates();
    CHECK(f.lib.templates().size() == 1);
}

TEST_CASE("a template's own picture is kept in its file") {
    Fixture f;
    const KindDef& k = f.lib.kinds()[0];
    std::optional<Template> t = f.lib.make(k, &k.presets[0], "");
    REQUIRE(t);
    const u32 plain = t->look();
    CHECK(f.lib.picture_file(*t).empty());
    t->picture = "Звезда.png";
    CHECK(t->look() != plain); // icons are drawn again
    CHECK(f.lib.picture_file(*t) == f.lib.pictures_folder() / utf8_path("Звезда.png"));
    REQUIRE(f.lib.put(*t));
    std::optional<Template> read = read_template(t->file);
    REQUIRE(read);
    CHECK(read->picture == "Звезда.png");
    CHECK(read->rev == t->rev); // a picture does not touch the copies' values
}

TEST_CASE("a template's picture of frames: read, written and looked at") {
    Fixture f;
    Template coins = *f.lib.find("coins");
    CHECK(coins.frames == 1);
    coins.picture = "монета.png";
    const u32 one = coins.look(), rev = coins.rev;
    coins.frames = 4;
    CHECK(coins.look() != one); // its icons are cut from frame 0 again
    REQUIRE(f.lib.put(coins));
    CHECK(f.lib.find("coins")->rev == rev); // the copies' values are as they were
    std::vector<u8> bytes;
    REQUIRE(read_file(coins.file, bytes));
    CHECK(std::string(bytes.begin(), bytes.end()).find("\"frames\": 4") != std::string::npos);
    std::optional<Template> read = read_template(coins.file);
    REQUIRE(read);
    CHECK(read->frames == 4);
    // One frame is not written; what is not 1..8 reads as one.
    coins.frames = 1;
    REQUIRE(f.lib.put(coins));
    REQUIRE(read_file(coins.file, bytes));
    CHECK(std::string(bytes.begin(), bytes.end()).find("frames") == std::string::npos);
    for (const char* bad : {"0", "9", "-2", "\"4\"", "2.5"}) {
        write_text(f.dir / "objects" / "odd.object.json",
                   std::string(R"({"id": "odd", "name": "Странный", "kind": "pickup", "picture": "x.png", "frames": )") + bad + "}");
        read = read_template(f.dir / "objects" / "odd.object.json");
        REQUIRE(read);
        CHECK_MESSAGE(read->frames == 1, bad);
    }
    write_text(f.dir / "objects" / "odd.object.json", R"({"id": "odd", "name": "Странный", "kind": "pickup", "frames": 8})");
    REQUIRE((read = read_template(f.dir / "objects" / "odd.object.json")));
    CHECK(read->frames == kMaxFrames);
    // Shared and taken back: another number of frames is another template.
    const auto shared_dir = temp_folder("forge_objects_frames_shared_test");
    Library shared;
    shared.set_pictures_folder(shared_dir / "pictures");
    REQUIRE(shared.load(f.lib.kinds_file(), shared_dir / "objects"));
    write_text(f.lib.pictures_folder() / utf8_path("монета.png"), "not really a png");
    coins.frames = 2;
    REQUIRE(f.lib.put(coins));
    std::optional<Template> up = shared.copy_from(f.lib, coins);
    REQUIRE(up);
    CHECK(up->frames == 2);
    REQUIRE(shared.put(*up));
    CHECK(shared.same_as(f.lib, coins));
    coins.frames = 3;
    REQUIRE(f.lib.put(coins));
    CHECK_FALSE(shared.same_as(f.lib, coins));
}

TEST_CASE("a template's animations: read, written, timed, and why one cannot play") {
    Fixture f;
    Template coins = *f.lib.find("coins");
    coins.picture = "монета.png";
    coins.frames = 4;
    REQUIRE(f.lib.put(coins));
    std::vector<u8> before;
    REQUIRE(read_file(coins.file, before));
    // None of its own: nothing written, the file as it was.
    CHECK(std::string(before.begin(), before.end()).find("animations") == std::string::npos);
    const u32 rev = coins.rev, look = coins.look();
    coins.animations["walk"] = {{1, 0, 0}, 4, true};
    coins.animations["air"] = {{3}, 7.5f, false};
    coins.animations["dance"] = {{2, 3}, 30, true}; // a state the module may not know: kept as it is
    REQUIRE(f.lib.put(coins));
    CHECK(f.lib.find("coins")->rev == rev);     // the copies' values are as they were
    CHECK(f.lib.find("coins")->look() == look); // and its icons (frame 0)
    std::vector<u8> bytes;
    REQUIRE(read_file(coins.file, bytes));
    const std::string text(bytes.begin(), bytes.end());
    CHECK(text.find("  \"animations\": {\n"
                    "    \"air\": {\"frames\": [3], \"fps\": 7.5, \"loop\": false},\n"
                    "    \"dance\": {\"frames\": [2, 3], \"fps\": 30},\n"
                    "    \"walk\": {\"frames\": [1, 0, 0], \"fps\": 4}\n"
                    "  },\n") != std::string::npos);
    std::optional<Template> read = read_template(coins.file);
    REQUIRE(read);
    CHECK(read->animations == coins.animations);
    CHECK(template_json(*read) == text); // read and written again: byte for byte
    // Taken away again: the file as it was before.
    coins.animations.clear();
    REQUIRE(f.lib.put(coins));
    REQUIRE(read_file(coins.file, bytes));
    CHECK(bytes == before);

    // The frame by the ticks since the state began: each frame a whole number of ticks (60 a second).
    const Clip walk{{1, 0, 0}, 4, true}, air{{3, 2}, 7.5f, false}, steps{{1, 2}, 10, true};
    CHECK(clip_ticks(4) == 15);
    CHECK(clip_ticks(7.5f) == 8);
    CHECK(clip_ticks(10) == 6);
    CHECK(clip_ticks(8) == 0);
    CHECK(clip_ticks(0) == 0);
    for (u64 t : {0u, 14u}) CHECK(clip_frame(walk, t) == 1);
    for (u64 t : {15u, 29u, 30u, 44u}) CHECK(clip_frame(walk, t) == 0);
    CHECK(clip_frame(walk, 45) == 1); // and over again
    CHECK(clip_frame(walk, 45 * 1000 + 15) == 0);
    CHECK(clip_frame(air, 7) == 3);
    CHECK(clip_frame(air, 8) == 2);
    CHECK(clip_frame(air, 8000) == 2); // not again: it stays on its last
    // The hero's steps as the game has always drawn them: 1 and 2 by turns every 6 ticks.
    for (u64 t = 0; t < 200; ++t) CHECK(clip_frame(steps, t) == 1u + static_cast<u32>((t / 6) & 1u));

    // Why one cannot play: the same words in the tab and the game.
    CHECK(clip_problem(walk, 2).empty());
    CHECK(clip_problem(walk, 1) == "кадра 2 нет в картинке из 1");
    CHECK(clip_problem({{}, 10, true}, 4) == "нет кадров");
    CHECK(clip_problem({std::vector<u32>(kMaxClipFrames + 1, 0), 10, true}, 4) == "кадров больше 16");
    CHECK(clip_problem({{0}, 8, true}, 4) == "кадров в секунду 8 нет в списке");
    // A file's animation that does not read: left out (the game draws that state by its rule), the others kept.
    write_text(f.dir / "objects" / "odd.object.json",
               R"({"id": "odd", "name": "Странный", "kind": "pickup", "frames": 4, "animations": {
                   "walk": {"frames": [1, 0], "fps": 5}, "a": {"frames": [], "fps": 5}, "b": {"frames": [1], "fps": 9},
                   "c": {"frames": ["1"], "fps": 5}, "d": {"frames": [-1], "fps": 5}, "e": [1], "f": {"fps": 5},
                   "g": {"frames": [0], "fps": 5, "loop": "no"}}})");
    REQUIRE((read = read_template(f.dir / "objects" / "odd.object.json")));
    CHECK(read->animations.size() == 2);
    CHECK(read->animations.count("walk"));
    CHECK(read->animations.count("g")); // a "loop" not true or false: it starts over, as by default
    CHECK(read->animations["g"].loop);
    // Shared and taken back: other animations are another template.
    const auto shared_dir = temp_folder("forge_objects_animations_shared_test");
    Library shared;
    shared.set_pictures_folder(shared_dir / "pictures");
    REQUIRE(shared.load(f.lib.kinds_file(), shared_dir / "objects"));
    write_text(f.lib.pictures_folder() / utf8_path("монета.png"), "not really a png");
    coins.animations["walk"] = walk;
    REQUIRE(f.lib.put(coins));
    std::optional<Template> up = shared.copy_from(f.lib, coins);
    REQUIRE(up);
    CHECK(up->animations == coins.animations);
    REQUIRE(shared.put(*up));
    CHECK(shared.same_as(f.lib, coins));
    coins.animations["walk"].fps = 5;
    REQUIRE(f.lib.put(coins));
    CHECK_FALSE(shared.same_as(f.lib, coins));
}

TEST_CASE("a kind may be one whose templates are not put on levels") {
    const auto dir = temp_folder("forge_objects_placed_test");
    write_text(dir / "kinds.json", R"({"kinds": [
        {"id": "pickup", "name": "Подбираемое", "components": {"ObjTestBody": {}}},
        {"id": "hero", "name": "Герой", "placed": false, "components": {}}]})");
    Library lib;
    REQUIRE(lib.load(dir / "kinds.json", dir / "objects"));
    REQUIRE(lib.kinds().size() == 2);
    CHECK(lib.kinds()[0].placed);
    CHECK_FALSE(lib.kinds()[1].placed);
}

TEST_CASE("a picture with new content under its name draws its templates again") {
    Fixture f;
    const KindDef& k = f.lib.kinds()[0];
    std::optional<Template> star = f.lib.make(k, &k.presets[0], "");
    REQUIRE(star);
    star->picture = "Звезда.png";
    REQUIRE(f.lib.put(*star));
    const Template* t = f.lib.find(star->id);
    const Template* coins = f.lib.find("coins");
    REQUIRE(t);
    REQUIRE(coins);
    const u32 look = t->look(), coins_look = coins->look(), rev = t->rev;
    const u64 version = f.lib.version();
    f.lib.picture_changed("Звезда.png"); // an asset of «Ресурсы» changed, its copy has the new content
    t = f.lib.find(star->id);
    CHECK(t->look() != look);                         // its icons are drawn again
    CHECK(t->rev == rev);                             // the copies' values are as they were
    CHECK(f.lib.find("coins")->look() == coins_look); // another picture's templates are not touched
    CHECK(f.lib.version() > version);
    // Read again from the files: still the new look; nothing of it is written.
    const u32 now = t->look();
    f.lib.reload_templates();
    REQUIRE(f.lib.find(star->id));
    CHECK(f.lib.find(star->id)->look() == now);
    std::optional<Template> read = read_template(f.lib.find(star->id)->file);
    REQUIRE(read);
    CHECK(read->picture == "Звезда.png");
    CHECK(read->picture_stamp == 0);
}

TEST_CASE("a pickup names its thing and its picture for the game's pages") {
    const auto dir = temp_folder("forge_objects_items_test");
    write_text(dir / "kinds.json", R"({
      "blocks": [
        {"id": "body", "name": "Тело", "components": {"ObjTestBody": {"half": 0.4}}},
        {"id": "pickup", "name": "Подбирается", "components": {"ObjTestPickup": {"count": 1}}, "needs": ["body"],
         "props": [{"id": "what", "name": "Что это", "bind": "ObjTestPickup.what",
                    "choices": [{"id": "coins", "name": "Монеты", "value": 1}]}]}
      ],
      "kinds": [
        {"id": "pickup", "name": "Подбираемое", "blocks": ["body", "pickup"]},
        {"id": "thing", "name": "Вещь", "blocks": ["body"]}
      ]
    })");
    write_text(dir / "objects" / "coins.object.json",
               R"({"id": "coins", "name": "Монеты", "kind": "pickup", "values": {"what": "coins"}, "picture": "монета.png"})");
    write_text(dir / "objects" / "box.object.json", R"({"id": "box", "name": "Ящик", "kind": "thing"})");
    Library lib;
    REQUIRE(lib.load(dir / "kinds.json", dir / "objects"));
    const Template& coins = *lib.find("coins");
    const Template& box = *lib.find("box");
    CHECK(lib.item_of(coins) == "coins");
    CHECK(lib.item_of(box).empty()); // not a pickup
    // The pictures folder is next to the kinds file: inside the game's folder.
    CHECK(lib.picture_in(coins, dir) == "pictures/монета.png");
    CHECK(lib.picture_in(box, dir).empty()); // no picture of its own
    // Pictures outside the game's folder are not reachable from its pages.
    CHECK(lib.picture_in(coins, dir / "objects").empty());
}

TEST_CASE("a template is copied between a game and the shared library") {
    Fixture f;
    const auto shared_dir = temp_folder("forge_objects_shared_test");
    Library shared;
    shared.set_pictures_folder(shared_dir / "pictures");
    shared.set_sounds_folder(shared_dir / "sounds");
    REQUIRE(shared.load(f.lib.kinds_file(), shared_dir / "objects"));
    CHECK(shared.templates().empty());

    // The game's coins, with a picture of their own, become shared.
    Template coins = *f.lib.find("coins");
    write_text(f.lib.pictures_folder() / "coin.png", "not really a png");
    coins.picture = "coin.png";
    REQUIRE(f.lib.put(coins));
    CHECK_FALSE(shared.same_as(f.lib, coins));
    std::optional<Template> up = shared.copy_from(f.lib, coins);
    REQUIRE(up);
    CHECK(up->key == coins.key); // the same object
    CHECK(up->file.parent_path() == shared_dir / "objects");
    REQUIRE(shared.put(*up));
    CHECK(std::filesystem::exists(shared_dir / "pictures" / "coin.png"));
    CHECK(shared.same_as(f.lib, coins));

    // Changed in the game: no longer the same, until it is shared again.
    Template more = f.lib.with_value(coins, "count", "25");
    REQUIRE(f.lib.put(more));
    CHECK_FALSE(shared.same_as(f.lib, more));
    REQUIRE(shared.put(*shared.copy_from(f.lib, more)));
    CHECK(shared.templates().size() == 1); // replaced, not added
    CHECK(shared.same_as(f.lib, more));

    // Another game takes it: a copy of its own, picture and all.
    const auto other_dir = temp_folder("forge_objects_other_game_test");
    Library other;
    other.set_pictures_folder(other_dir / "pictures");
    other.set_sounds_folder(other_dir / "sounds");
    REQUIRE(other.load(f.lib.kinds_file(), other_dir / "objects"));
    const Template& s = shared.templates()[0];
    REQUIRE(other.put(*other.copy_from(shared, s)));
    CHECK(other.same_as(shared, s));
    CHECK(other.value(*other.find("coins"), *other.prop_of(*other.find("coins"), "count")) == "25");
    CHECK(std::filesystem::exists(other_dir / "pictures" / "coin.png"));

    // Its own sound goes along too, to the shared library and from it.
    write_text(f.lib.sounds_folder() / utf8_path("звон.ogg"), "not really a sound");
    Template ringing = f.lib.with_value(more, "ding", "\"звон.ogg\"");
    REQUIRE(f.lib.put(ringing));
    CHECK_FALSE(shared.same_as(f.lib, ringing));
    REQUIRE(shared.put(*shared.copy_from(f.lib, ringing)));
    CHECK(std::filesystem::exists(shared.sounds_folder() / utf8_path("звон.ogg")));
    CHECK(shared.same_as(f.lib, ringing));
    REQUIRE(other.put(*other.copy_from(shared, shared.templates()[0])));
    CHECK(std::filesystem::exists(other_dir / "sounds" / utf8_path("звон.ogg")));
    // A different file of the same name there: same_as notices.
    write_text(other_dir / "sounds" / utf8_path("звон.ogg"), "another sound");
    CHECK_FALSE(other.same_as(shared, shared.templates()[0]));

    // A kind the game does not have cannot come in.
    Template odd = s;
    odd.kind = "dragon";
    std::string error;
    CHECK_FALSE(other.copy_from(shared, odd, &error));
    CHECK(error.find("dragon") != std::string::npos);
}

TEST_CASE("copies follow their template, but keep their own values") {
    PoolScope pool;
    Fixture f;
    World w(tight_desc(), std::make_shared<TopDownGenerator>(1));
    Scene s(w);
    s.register_component<ObjTestPickup>();
    s.register_component<ObjTestBody>();
    f.lib.attach(s);
    settle(w, s, kHome);

    const Template& coins = *f.lib.find("coins");
    const KindDef& k = *f.lib.kind_of(coins);
    flecs::entity a = f.lib.spawn(s, coins, 10.5, 20.0);
    flecs::entity b = f.lib.spawn(s, coins, 12.5, 20.0);
    REQUIRE(a.is_valid());
    CHECK(a.get<ObjTestPickup>().what == 1);
    CHECK(a.get<ObjTestPickup>().count == 10);
    CHECK(a.get<ObjTestBody>().half == doctest::Approx(0.3f)); // the kind's starting value
    CHECK(a.get<Position>().tile_y() == doctest::Approx(20.0 - 0.3 - 0.02));
    CHECK(f.lib.template_of(a) == &coins);

    // b is a pile of twelve: its own value.
    CHECK(f.lib.set_value(s, b, *k.prop("count"), "12"));
    CHECK(b.get<ObjectRef>().overrides_prop("count"));

    // The template changes: a follows, b keeps its count but takes the rest.
    Template changed = f.lib.with_value(coins, "count", "20");
    changed = f.lib.with_value(changed, "what", "\"gem\"");
    REQUIRE(f.lib.put(changed));
    CHECK(f.lib.refresh(s) == 2);
    CHECK(a.get<ObjTestPickup>().count == 20);
    CHECK(a.get<ObjTestPickup>().what == 2);
    CHECK(b.get<ObjTestPickup>().count == 12);
    CHECK(b.get<ObjTestPickup>().what == 2);
    CHECK(f.lib.refresh(s) == 0); // all up to date

    // Setting the template's value again ends the override.
    CHECK(f.lib.set_value(s, b, *k.prop("count"), "20"));
    CHECK(!b.get<ObjectRef>().overrides_prop("count"));
}

TEST_CASE("copies that were away catch up when their chunk loads") {
    PoolScope pool;
    Fixture f;
    World w(tight_desc(), std::make_shared<TopDownGenerator>(1));
    Scene s(w);
    s.register_component<ObjTestPickup>();
    s.register_component<ObjTestBody>();
    f.lib.attach(s);
    settle(w, s, kHome);
    flecs::entity a = f.lib.spawn(s, *f.lib.find("coins"), 10.5, 20.0);
    f.lib.set_value(s, a, *f.lib.kinds()[0].prop("what"), "\"gem\"");
    settle(w, s, kAway); // packed with its chunk
    CHECK(!a.is_alive());

    REQUIRE(f.lib.put(f.lib.with_value(*f.lib.find("coins"), "count", "33")));
    settle(w, s, kHome);
    u32 found = 0;
    s.ecs().each([&](const ObjTestPickup& p, const ObjectRef& r) {
        ++found;
        CHECK(p.count == 33);
        CHECK(p.what == 2); // its own, kept through the save
        CHECK(r.overrides == "what");
    });
    CHECK(found == 1);
}

TEST_CASE("objects made before templates are adopted with their own values") {
    PoolScope pool;
    Fixture f;
    World w(tight_desc(), std::make_shared<TopDownGenerator>(1));
    Scene s(w);
    s.register_component<ObjTestPickup>();
    s.register_component<ObjTestBody>();
    f.lib.attach(s);
    f.lib.set_adopt([&](flecs::entity e) { return e.has<ObjTestPickup>() ? f.lib.find("coins") : nullptr; });
    settle(w, s, kHome);
    flecs::entity old = s.spawn(Position::at_tile(5, 5));
    old.set<ObjTestPickup>({1, 12, false});
    const std::vector<u8> bytes = s.pack(old);
    old.destruct();
    flecs::entity back = s.unpack(bytes);
    REQUIRE(back.is_valid());
    REQUIRE(back.has<ObjectRef>());
    CHECK(back.get<ObjectRef>().overrides == "count");
    CHECK(back.get<ObjTestPickup>().count == 12);
}

TEST_CASE("objects are put together from blocks") {
    PoolScope pool;
    const auto dir = temp_folder("forge_objects_blocks_test");
    write_text(dir / "kinds.json", R"({
      "blocks": [
        {"id": "body", "name": "Тело", "components": {"ObjTestBody": {"half": 0.4}}, "excludes": ["solid"]},
        {"id": "solid", "name": "Физика", "components": {"ObjTestBody": {"half": 0.5}}, "excludes": ["body"]},
        {"id": "pickup", "name": "Подбирается", "components": {"ObjTestPickup": {"count": 1}}, "needs": ["body"],
         "props": [{"id": "count", "name": "Сколько", "bind": "ObjTestPickup.count"}], "was": ["loot"]}
      ],
      "kinds": [
        {"id": "pickup", "name": "Подбираемое", "foot": 0.3, "blocks": ["body", "pickup"],
         "components": {"ObjTestBody": {"half": 0.3}}},
        {"id": "thing", "name": "Вещь", "blocks": ["body"]}
      ]
    })");
    write_text(dir / "objects" / "coins.object.json",
               R"({"id": "coins", "name": "Монеты", "kind": "pickup", "values": {"count": 10}})");
    // Written when «Подбирается» was called "loot".
    write_text(dir / "objects" / "old.object.json",
               R"({"id": "old", "name": "Старое", "kind": "thing", "blocks": ["body", "loot"]})");
    Library lib;
    REQUIRE(lib.load(dir / "kinds.json", dir / "objects"));
    REQUIRE(lib.blocks().size() == 3);
    CHECK(lib.has_block(*lib.find("old"), "pickup")); // a renamed block is found by its former id
    const Template& coins = *lib.find("coins");
    CHECK(lib.blocks_of(coins).size() == 2);
    CHECK(lib.prop_of(coins, "count") != nullptr);
    CHECK(lib.value(coins, *lib.prop_of(coins, "count")) == "10");

    // A thing without «Подбирается» gains it, and loses it again.
    std::optional<Template> thing = lib.make(*lib.kind("thing"), nullptr, "Вещь");
    REQUIRE(thing);
    CHECK(lib.props_of(*thing).empty());
    Template with = lib.with_block(*thing, "pickup", true);
    CHECK(lib.has_block(with, "pickup"));
    CHECK(with.rev != thing->rev); // copies notice
    CHECK(lib.value(with, *lib.prop_of(with, "count")) == "1"); // the block's starting value
    // «Физика» replaces «Тело», and «Подбирается» needs a body: it goes too.
    Template solid = lib.with_block(with, "solid", true);
    CHECK(solid.blocks == std::vector<std::string>{"solid"});
    // Taking «Тело» away takes «Подбирается».
    CHECK(lib.with_block(with, "body", false).blocks.empty());

    // Copies get and lose the blocks' components.
    World w(tight_desc(), std::make_shared<TopDownGenerator>(1));
    Scene s(w);
    s.register_component<ObjTestPickup>();
    s.register_component<ObjTestBody>();
    lib.attach(s);
    settle(w, s, kHome);
    flecs::entity a = lib.spawn(s, coins, 10.5, 20.0);
    REQUIRE(a.is_valid());
    CHECK(a.get<ObjTestBody>().half == doctest::Approx(0.3f)); // the kind's own starting value
    CHECK(a.get<ObjTestPickup>().count == 10);
    Template no_pickup = lib.with_block(coins, "pickup", false);
    CHECK(no_pickup.values.empty()); // «Сколько» went with its block
    REQUIRE(lib.put(no_pickup));
    CHECK(lib.refresh(s) == 1);
    CHECK(!a.has<ObjTestPickup>());
    CHECK(a.has<ObjTestBody>());
    REQUIRE(lib.put(lib.with_block(*lib.find("coins"), "pickup", true)));
    CHECK(lib.refresh(s) == 1);
    CHECK(a.get<ObjTestPickup>().count == 1);
    // The blocks are in the file.
    CHECK(read_template(lib.find("coins")->file)->blocks == std::vector<std::string>{"body", "pickup"});
}
