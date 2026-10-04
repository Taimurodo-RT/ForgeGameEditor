#include "forge/core/file.h"
#include "forge/core/jobs.h"
#include "forge/logic/logic.h"
#include "forge/objects/library.h"
#include "forge/scene/scene.h"
#include "forge/world/generators.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <memory>
#include <string>

using namespace forge;
using namespace forge::logic;

namespace {

struct PoolScope {
    PoolScope() { jobs::init(3); }
    ~PoolScope() { jobs::shutdown(); }
};

const char* kVerbs = R"({
  // comments and trailing commas are fine
  "verbs": [
    { "id": "open", "name": "открывает", "plural": "открывают", "icon": "key", "case": "acc",
      "when": "touch", "touch": "b", "needs": "a", "do": "open", "target": "b", "sound": "open",
      "a": "thing", "b": "thing", "fail": "Нужен предмет «{a}»", "a_has": "pickup", "b_has": ["door"],
      "step": "Открыть {b:acc}",
      "about": "Когда герой с {a:ins} подходит к {b:dat}, {b} открывается." },
    { "id": "hurt", "name": "ранит", "plural": "ранят", "case": "acc", "when": "touch", "touch": "a",
      "do": "hurt", "target": "b", "a": "thing", "b": "hero", "about": "Герой теряет сердце." },
    { "id": "follow", "name": "идёт за", "plural": "идут за", "case": "ins", "when": "always",
      "do": "follow", "target": "a", "a": "thing", "b": "hero", "about": "{a} идёт за героем." },
  ]
})";

Thing thing(const char* id, const char* name, bool animate = false) {
    Thing t;
    t.id = id;
    t.name = name;
    t.animate = animate;
    t.plural = looks_plural(name);
    t.forms = decline(name, animate);
    return t;
}

struct Words {
    std::vector<Thing> things{hero_thing(), thing("key", "Ключ"), thing("door", "Дверь"), thing("spikes", "Шипы"),
                              thing("fox", "Лиса", true)};
    FindThing find = [this](std::string_view id) -> const Thing* {
        for (const Thing& t : things)
            if (t.id == id) return &t;
        return nullptr;
    };
};

std::filesystem::path temp_folder(const char* name) {
    const auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

void write_text(const std::filesystem::path& p, const std::string& text) {
    REQUIRE(write_file_atomic(p, std::span(reinterpret_cast<const u8*>(text.data()), text.size())));
}

} // namespace

TEST_CASE("names take their forms in phrases") {
    CHECK(decline("Дверь", false).acc == "Дверь");
    CHECK(decline("Дверь", false).dat == "Двери");
    CHECK(decline("Ключ", false).ins == "Ключом");
    CHECK(decline("Лиса", true).acc == "Лису");
    CHECK(decline("Монеты", false).gen == "Монет");
    CHECK(decline("Старый ключ", false).ins == "Старым ключом");
    CHECK(decline("Ключ от кузницы", false).ins == "Ключом от кузницы");
    CHECK(decline("Зверёк", true).acc == "Зверька");
    CHECK(looks_plural("Шипы"));
    CHECK_FALSE(looks_plural("Ключ"));

    Verbs verbs;
    REQUIRE(verbs.parse(kVerbs));
    Words w;
    Link l{1, "key", "open", "door"};
    CHECK(phrase(l, *verbs.find("open"), *w.find("key"), *w.find("door")) == "Ключ открывает Дверь");
    Link spikes{2, "spikes", "hurt", "hero"};
    CHECK(phrase(spikes, *verbs.find("hurt"), *w.find("spikes"), *w.find("hero")) == "Шипы ранят героя");
    l.hint = true;
    l.once = true;
    CHECK(meaning(l, *verbs.find("open"), *w.find("key"), *w.find("door")) ==
          "Когда герой с Ключом подходит к Двери, Дверь открывается. Уточнено: только один раз, иначе подсказка «Нужен предмет «Ключ»».");
}

TEST_CASE("verbs are offered for things that have their blocks") {
    Verbs verbs;
    REQUIRE(verbs.parse(kVerbs));
    Words w;
    Thing key = *w.find("key"), door = *w.find("door");
    const VerbDef& open = *verbs.find("open");
    CHECK(open.a_has == std::vector<std::string>{"pickup"});
    CHECK_FALSE(suits(open, key, door)); // no blocks yet
    key.blocks = {"body", "pickup"};
    door.blocks = {"door"};
    CHECK(suits(open, key, door));
    CHECK_FALSE(suits(open, door, key));
    CHECK_FALSE(suits(open, *w.find("hero"), door));
    const VerbDef& follow = *verbs.find("follow");
    CHECK(suits(follow, *w.find("fox"), *w.find("hero")));
    CHECK_FALSE(suits(follow, *w.find("hero"), *w.find("fox")));
}

TEST_CASE("a link reads as steps") {
    Verbs verbs;
    REQUIRE(verbs.parse(kVerbs));
    Words w;
    Link l{1, "key", "open", "door"};
    l.sound = true;
    l.hint = true;
    std::vector<Step> st = steps(l, *verbs.find("open"), *w.find("key"), *w.find("door"));
    REQUIRE(st.size() == 5);
    CHECK(st[0].part == "when");
    CHECK(st[0].text == "Когда герой касается Двери");
    CHECK(st[1].text == "Если у героя есть «Ключ»");
    CHECK(st[1].refine.empty());
    CHECK(st[2].text == "Открыть Дверь");
    CHECK(st[3].refine == "sound");
    CHECK(st[4].part == "else");
    CHECK(st[4].refine == "hint");
    l.night = true;
    l.once = true;
    st = steps(l, *verbs.find("open"), *w.find("key"), *w.find("door"));
    CHECK(st.size() == 7);
    CHECK(st[1].refine == "night");
    // What «Добавить шаг» offers: the refinements not yet on.
    l = Link{2, "spikes", "hurt", "hero"};
    const std::vector<Refine> r = refinements(l, *verbs.find("hurt"), *w.find("spikes"), *w.find("hero"));
    REQUIRE(r.size() == 2); // no sound, no hint: nothing is needed
    CHECK(r[0].id == "night");
    CHECK(steps(l, *verbs.find("hurt"), *w.find("spikes"), *w.find("hero"))[0].text == "Когда герой касается Шипов");
}

TEST_CASE("links and verbs read and write json") {
    Verbs verbs;
    REQUIRE(verbs.parse(kVerbs));
    REQUIRE(verbs.all().size() == 3);
    const VerbDef* open = verbs.find("open");
    REQUIRE(open);
    CHECK(open->needs == "a");
    CHECK(open->touch == Side::B);
    CHECK(verbs.find("follow")->always);
    CHECK(verbs.find("follow")->object_case == "ins");

    Logic logic;
    REQUIRE(logic.parse(R"({"links": [{"id": 4, "a": "key", "verb": "open", "b": "door", "hint": true},
                                      {"a": "spikes", "verb": "hurt", "b": "hero"}],
                           "board": {"key": [10, 20]}})"));
    REQUIRE(logic.links.size() == 2);
    CHECK(logic.links[0].hint);
    CHECK(logic.links[1].id == 5); // no id: a fresh one
    CHECK(logic.spot("key")->y == 20);
    const u32 id = logic.add({0, "fox", "follow", "hero"});
    CHECK(id == 6);
    logic.set_spot("fox", 140, 2);

    Logic again;
    REQUIRE(again.parse(logic.json()));
    CHECK(again.links.size() == 3);
    CHECK(again.find(6)->verb == "follow");
    CHECK(again.spot("fox")->x == 140);
    CHECK(again.remove(4));
    CHECK_FALSE(again.find(4));

    Logic empty;
    CHECK(empty.load(temp_folder("forge_logic_missing") / "logic.json")); // a game without links yet
    CHECK(empty.links.empty());
}

TEST_CASE("links compile to one module per listening thing") {
    Verbs verbs;
    REQUIRE(verbs.parse(kVerbs));
    Words w;
    Logic logic;
    logic.add({0, "key", "open", "door", false, false, true, true});
    logic.add({0, "spikes", "hurt", "hero"});
    logic.add({0, "fox", "follow", "hero"});
    logic.add({0, "hero", "follow", "fox"}); // the hero cannot be the follower
    logic.add({0, "key", "fly", "door"});    // no such verb
    const Compiled c = compile(logic, verbs, w.find);
    REQUIRE(c.modules.size() == 3);
    REQUIRE(c.problems.size() == 2);
    CHECK(c.problems[0].link == 4);
    CHECK(c.problems[1].text.find("fly") != std::string::npos);

    const Module* door = c.find("door");
    REQUIRE(door);
    CHECK(door->name == "logic:door");
    CHECK(door->touch);
    CHECK(door->source.find("-- Ключ открывает Дверь") != std::string::npos);
    CHECK(door->source.find("logic.has(hero, \"key\")") != std::string::npos);
    CHECK(door->source.find("logic.hint(hero, \"Нужен предмет «Ключ»\")") != std::string::npos);
    CHECK(door->source.find("logic.sound(self, \"open\")") != std::string::npos);
    // Every line of a link's code knows the link.
    const usize at = door->source.find("logic.act");
    const i32 line = static_cast<i32>(std::count(door->source.begin(), door->source.begin() + static_cast<long>(at), '\n')) + 1;
    CHECK(door->map.node_at(line) == 0);

    const Module* spikes = c.find("spikes");
    REQUIRE(spikes);
    CHECK(spikes->source.find("local target = hero") != std::string::npos);
    CHECK(spikes->source.find("logic.act(\"hurt\", target, \"hero\", self, hero)") != std::string::npos);
    const Module* fox = c.find("fox");
    REQUIRE(fox);
    CHECK_FALSE(fox->touch);
    CHECK(fox->source.find("function S.on_start(self)") != std::string::npos);

    script::SourceMap map;
    const std::string all = listing(logic, verbs, w.find, &map);
    CHECK(all.find("-- не работает:") != std::string::npos);
    CHECK(map.line_node.size() == static_cast<usize>(std::count(all.begin(), all.end(), '\n')));
}

namespace {

// A game that writes down what links ask of it.
struct NoteGame final : Game {
    flecs::entity_t hero_e = 0;
    bool carries_key = false;
    std::vector<std::string> acts, hints, sounds;
    std::vector<u32> fired_links;
    flecs::entity_t last_target = 0;

    bool is_hero(flecs::entity_t e) override { return e && e == hero_e; }
    flecs::entity_t hero() override { return hero_e; }
    bool has(flecs::entity_t, std::string_view thing) override { return carries_key && thing == "key"; }
    bool act(std::string_view action, flecs::entity_t target, std::string_view thing, flecs::entity_t,
             flecs::entity_t) override {
        acts.push_back(std::string(action) + " " + std::string(thing));
        last_target = target;
        return true;
    }
    void hint(flecs::entity_t, std::string_view text) override { hints.emplace_back(text); }
    void sound(flecs::entity_t, std::string_view cue) override { sounds.emplace_back(cue); }
    bool night() override { return false; }
    void fired(u32 link) override { fired_links.push_back(link); }
};

class EmptyGenerator final : public world::Generator {
public:
    void generate(world::ChunkCoord, const world::ChunkTiles& out) const override {
        for (u32 l = 0; l < out.layer_count; ++l)
            for (u32 i = 0; i < world::kChunkTiles; ++i) out.layer(l)[i] = 0;
    }
};

} // namespace

TEST_CASE("the hero touching a door with the key opens it") {
    PoolScope pool;
    const auto dir = temp_folder("forge_logic_runtime");
    write_text(dir / "kinds.json", R"({"kinds": [{"id": "thing", "name": "Вещь", "group": "Разное", "icon": "box", "foot": 0.5}]})");
    write_text(dir / "objects" / "key.object.json", R"({"id": "key", "name": "Ключ", "kind": "thing"})");
    write_text(dir / "objects" / "door.object.json", R"({"id": "door", "name": "Дверь", "kind": "thing"})");
    objects::Library library;
    REQUIRE(library.load(dir / "kinds.json", dir / "objects"));

    world::World world(world::WorldDesc{}, std::make_shared<EmptyGenerator>());
    scene::Scene scene(world);
    sim::Simulation sim(world, scene);
    script::ScriptHost scripts(sim, scene);
    library.attach(scene);
    NoteGame game;
    Runtime runtime(scripts, library, game); // before the scripts first run
    const world::Rect view{-64, -64, 128, 64};
    auto run = [&](u32 ticks) {
        for (u32 i = 0; i < ticks; ++i) sim.update(1.0 / 60.0, view);
    };
    sim.update(0, view);
    world.finish_loading();
    run(1);

    Verbs verbs;
    REQUIRE(verbs.parse(kVerbs));
    Logic logic;
    logic.add({0, "key", "open", "door", false, false, true, true});
    std::vector<Problem> problems;
    REQUIRE(runtime.load(logic, verbs, &problems));
    CHECK(problems.empty());
    runtime.attach(scene);

    // A copy made after attach gets the door's script and a touch trigger.
    flecs::entity door = library.spawn(scene, *library.find("door"), 10.5, 6);
    REQUIRE(door.is_valid());
    const script::Script* s = door.try_get<script::Script>();
    REQUIRE(s);
    CHECK(s->name == "logic:door");
    CHECK(door.has<sim::Trigger>());
    // The key itself does not listen.
    flecs::entity key = library.spawn(scene, *library.find("key"), 30.5, 6);
    CHECK_FALSE(key.has<script::Script>());

    flecs::entity hero = scene.spawn(scene::Position::at_tile(20, 5.5));
    game.hero_e = hero.id();
    run(3);
    CHECK(game.acts.empty());

    // Without the key: a hint, nothing opens.
    hero.set<scene::Position>(scene::Position::at_tile(11, 5.5));
    run(3);
    CHECK(game.acts.empty());
    REQUIRE(game.hints.size() == 1);
    CHECK(game.hints[0] == "Нужен предмет «Ключ»");

    // With the key: it opens, with its sound, and the editor hears which link.
    hero.set<scene::Position>(scene::Position::at_tile(20, 5.5));
    run(3);
    game.carries_key = true;
    hero.set<scene::Position>(scene::Position::at_tile(11, 5.5));
    run(3);
    REQUIRE(game.acts.size() == 1);
    CHECK(game.acts[0] == "open door");
    CHECK(game.last_target == door.id());
    CHECK(game.sounds == std::vector<std::string>{"open"});
    CHECK(game.fired_links == std::vector<u32>{1});
    CHECK(scripts.errors().empty());

    // New links replace the old ones while the game runs: the door stops
    // listening and gives back its script and trigger.
    Logic none;
    REQUIRE(runtime.load(none, verbs));
    CHECK_FALSE(door.has<script::Script>());
    CHECK_FALSE(door.has<sim::Trigger>());
}
