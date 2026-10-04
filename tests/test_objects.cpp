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
    CHECK(k.props.size() == 3); // the one bound to a missing field is dropped
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
