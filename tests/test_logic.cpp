#include "forge/core/file.h"
#include "forge/core/jobs.h"
#include "forge/core/path.h"
#include "forge/logic/logic.h"
#include "forge/objects/library.h"
#include "forge/script/compiler.h"
#include "forge/scene/scene.h"
#include "forge/world/generators.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <span>
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

TEST_CASE("ideas name a verb and the fields an author picks") {
    Ideas ideas;
    REQUIRE(ideas.parse(R"({"ideas": [
      {"id": "door_key", "name": "Дверь с ключом", "verb": "open", "icon": "key",
       "fields": [{"side": "b", "label": "Дверь"}, {"side": "a", "label": "Открывает"}],
       "refine": {"hint": true, "sound": true}},
      {"id": "broken", "name": "Без действия"},
      {"id": "trap", "name": "Ловушка", "verb": "hurt", "fields": [{"side": "a", "label": "Что ранит"}]}
    ]})"));
    REQUIRE(ideas.all().size() == 2); // one without a verb is left out
    const Idea* d = ideas.of_verb("open");
    REQUIRE(d);
    CHECK(d->name == "Дверь с ключом");
    REQUIRE(d->fields.size() == 2);
    CHECK(d->fields[0].side == Side::B);
    CHECK(d->fields[1].label == "Открывает");
    CHECK(d->hint);
    CHECK_FALSE(d->once);
    CHECK(ideas.find("trap")->verb == "hurt");
    CHECK(ideas.of_verb("collect") == nullptr);
    Ideas none;
    CHECK(none.load(temp_folder("forge_logic_ideas") / "ideas.json")); // no file: no ideas
    CHECK(none.all().empty());
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

TEST_CASE("a link with its own code runs the code instead of its verb") {
    Verbs verbs;
    REQUIRE(verbs.parse(kVerbs));
    Words w;
    Logic logic;
    logic.add({0, "key", "open", "door", false, false, true, true});
    // The code to start from: the verb's action with the link's refinements.
    const std::string start = default_code(logic.links[0], verbs, w.find);
    CHECK(start.find("logic.act(\"open\", target") != std::string::npos);
    CHECK(start.find("logic.hint(hero") != std::string::npos);
    CHECK(start.find("logic.fired") == std::string::npos);
    CHECK(script::check_syntax(start));
    std::string error;
    CHECK_FALSE(script::check_syntax("if then", &error));
    CHECK_FALSE(error.empty());

    logic.links[0].code = "logic.act(\"open\", target, \"door\", self, hero)\nlogic.sound(self, \"bell\")\n";
    CHECK(code_lines(logic.links[0].code) == 2);
    Logic back;
    REQUIRE(back.parse(logic.json()));
    CHECK(back.links[0].code == logic.links[0].code);

    const Compiled c = compile(logic, verbs, w.find);
    const Module* door = c.find("door");
    REQUIRE(door);
    CHECK(door->source.find("(свой код)") != std::string::npos);
    CHECK(door->source.find("    logic.sound(self, \"bell\")") != std::string::npos);
    CHECK(door->source.find("logic.fired(1)") != std::string::npos);
    CHECK(door->source.find("logic.hint") == std::string::npos);
    CHECK(script::check_syntax(door->source));

    const VerbDef& open = *verbs.find("open");
    const std::vector<Step> st = steps(logic.links[0], open, w.things[1], w.things[2]);
    REQUIRE(st.size() == 2);
    CHECK(st[1].text == "Свой код: 2 строки");
    CHECK(st[1].refine == "code");
    CHECK(refinements(logic.links[0], open, w.things[1], w.things[2]).empty());
}

TEST_CASE("a link reads as a scheme of nodes and back") {
    Verbs verbs;
    REQUIRE(verbs.parse(kVerbs));
    Words w;
    script::ScriptApi api;
    script::register_core_api(api);
    const script::NodeLibrary nodes = node_library(api);
    REQUIRE(nodes.find("logic.act"));
    REQUIRE(nodes.find("std.flow.if"));
    REQUIRE(nodes.find("api.wait"));

    Logic logic;
    logic.add({0, "key", "open", "door", false, false, true, true});
    Link& l = logic.links[0];
    // «Когда» → «Если» (the key) → «Сделать» → «Звук», and «Подсказка» otherwise.
    script::Graph g = scheme_of(l, verbs, w.find);
    CHECK(g.nodes.size() == 6);
    REQUIRE(when_node(g));
    auto count = [&](const script::Graph& graph, std::string_view def) {
        return std::count_if(graph.nodes.begin(), graph.nodes.end(), [&](const script::GraphNode& n) { return n.def == def; });
    };
    CHECK(count(g, "logic.has") == 1);
    CHECK(count(g, "logic.hint") == 1);

    // Moving nodes changes nothing: the link stays plain, its nodes stay put.
    for (script::GraphNode& n : g.nodes) n.x += 40;
    set_scheme(l, g, verbs, w.find);
    CHECK_FALSE(own_scheme(l, verbs, w.find));
    CHECK(l.sound);
    CHECK(scheme_of(l, verbs, w.find).find(when_node(g))->x == 40);
    // A refinement from another mode: the scheme follows it.
    l.night = true;
    CHECK(count(scheme_of(l, verbs, w.find), "logic.night") == 1);
    l.night = false;

    // Taking the sound away is the «Со звуком» refinement off.
    for (const script::GraphNode& n : g.nodes)
        if (n.def == "logic.sound") {
            g.remove(n.uid);
            break;
        }
    set_scheme(l, g, verbs, w.find);
    CHECK_FALSE(own_scheme(l, verbs, w.find));
    CHECK_FALSE(l.sound);
    CHECK(l.hint);

    // A node no refinement makes: the link keeps its scheme.
    u32 act = 0;
    for (const script::GraphNode& n : g.nodes)
        if (n.def == "logic.act") act = n.uid;
    REQUIRE(act);
    script::GraphNode& coins = g.add("std.var.add", 900, 0);
    coins.set_value("name", "opened");
    g.link(act, script::kFlowNext, coins.uid);
    set_scheme(l, g, verbs, w.find);
    CHECK(own_scheme(l, verbs, w.find));
    CHECK(scheme_nodes(l) == 6);
    const VerbDef& open = *verbs.find("open");
    const std::vector<Step> st = steps(l, open, w.things[1], w.things[2]);
    REQUIRE(st.size() == 2);
    CHECK(st[1].text == "Уточнено в Схеме: 6 нод");
    CHECK(st[1].refine == "graph");
    CHECK(refinements(l, open, w.things[1], w.things[2]).empty());
    CHECK(scheme_of(l, verbs, w.find).nodes.size() == 6);

    Logic back;
    REQUIRE(back.parse(logic.json()));
    CHECK(scheme_nodes(back.links[0]) == 6);

    // Without the nodes the link cannot be built; with them it is code.
    CHECK(compile(logic, verbs, w.find).modules.empty());
    const Compiled c = compile(logic, verbs, w.find, &nodes);
    CHECK(c.problems.empty());
    const Module* door = c.find("door");
    REQUIRE(door);
    CHECK(door->source.find("(схема)") != std::string::npos);
    CHECK(door->source.find("S.on_enter[#S.on_enter + 1] = function(self, other)") != std::string::npos);
    CHECK(door->source.find("forge.set_var(self, \"opened\"") != std::string::npos);
    CHECK(door->source.find("logic.has(") != std::string::npos);
    CHECK(script::check_syntax(door->source));
    CHECK(door->map.line_node.size() == static_cast<usize>(std::count(door->source.begin(), door->source.end(), '\n')));

    // A broken scheme says which node.
    script::Graph bad = scheme_of(l, verbs, w.find);
    script::GraphNode& hint = bad.add("logic.hint", 900, 300);
    hint.set_value("text", "");
    bad.link(coins.uid, script::kFlowNext, hint.uid);
    l.graph = bad.to_json();
    const Compiled broken = compile(logic, verbs, w.find, &nodes);
    CHECK(broken.modules.empty());
    const auto about_hint = std::find_if(broken.problems.begin(), broken.problems.end(),
                                         [&](const Problem& p) { return p.node == hint.uid; });
    REQUIRE(about_hint != broken.problems.end());
    CHECK_FALSE(about_hint->warning);
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

TEST_CASE("a link's scheme runs in the game") {
    PoolScope pool;
    const auto dir = temp_folder("forge_logic_scheme");
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
    Runtime runtime(scripts, library, game);
    const world::Rect view{-64, -64, 128, 64};
    auto run = [&](u32 ticks) {
        for (u32 i = 0; i < ticks; ++i) sim.update(1.0 / 60.0, view);
    };
    sim.update(0, view);
    world.finish_loading();
    run(1);

    Verbs verbs;
    REQUIRE(verbs.parse(kVerbs));
    Words w;
    Logic logic;
    logic.add({0, "key", "open", "door", false, false, false, false});
    // No key needed any more: «Когда» → «Сделать» → «Подсказка».
    script::Graph g;
    const u32 when = g.add("logic.when", 0, 0).uid;
    script::GraphNode& act = g.add("logic.act", 280, 0);
    act.set_value("action", "open");
    act.set_value("thing", "door");
    g.link(when, "b", act.uid, "target");
    g.link(when, script::kFlowNext, act.uid);
    script::GraphNode& hint = g.add("logic.hint", 560, 0);
    hint.set_value("text", "Открыто без ключа");
    g.link(act.uid, script::kFlowNext, hint.uid);
    set_scheme(logic.links[0], g, verbs, w.find);
    REQUIRE(own_scheme(logic.links[0], verbs, w.find));

    std::vector<Problem> problems;
    REQUIRE(runtime.load(logic, verbs, &problems));
    CHECK(problems.empty());
    runtime.attach(scene);
    flecs::entity door = library.spawn(scene, *library.find("door"), 10.5, 6);
    REQUIRE(door.is_valid());
    flecs::entity hero = scene.spawn(scene::Position::at_tile(20, 5.5));
    game.hero_e = hero.id();
    run(3);
    hero.set<scene::Position>(scene::Position::at_tile(11, 5.5));
    run(3);
    REQUIRE(game.acts.size() == 1);
    CHECK(game.acts[0] == "open door");
    CHECK(game.last_target == door.id());
    CHECK(game.hints == std::vector<std::string>{"Открыто без ключа"});
    CHECK(game.fired_links == std::vector<u32>{1});
    CHECK(scripts.errors().empty());
}

TEST_CASE("a thing's own scheme reads, writes and compiles") {
    Verbs verbs;
    REQUIRE(verbs.parse(kVerbs));
    Words w;
    script::ScriptApi api;
    script::register_core_api(api);
    const script::NodeLibrary nodes = node_library(api);

    Logic logic;
    logic.add({0, "key", "open", "door"});
    ThingScheme own;
    own.thing = "door";
    script::Graph g = new_thing_scheme("door");
    REQUIRE(g.nodes.size() == 2);
    const u32 tick = g.nodes[1].uid;
    // Каждый шаг → Сдвинуть (этот объект, на dt по X).
    const u32 self = g.add("std.self", 0, 300).uid;
    const u32 move = g.add("api.entity.move", 280, 190).uid;
    g.link(tick, script::kFlowNext, move);
    g.link(self, "actor", move, "actor");
    g.link(tick, "dt", move, "dx");
    own.graph = g.to_json();
    const u32 id = logic.add_scheme(own);
    CHECK(id == 2);
    CHECK(logic.add_scheme(own) == 0); // one per thing
    ThingScheme for_hero;
    for_hero.thing = std::string(kHero);
    CHECK(logic.add_scheme(for_hero) == 0);

    Logic again;
    REQUIRE(again.parse(logic.json()));
    REQUIRE(again.schemes.size() == 1);
    CHECK(again.scheme_for("door")->id == id);
    CHECK(again.next_id == 3);
    script::Graph back;
    REQUIRE(back.from_json(again.schemes[0].graph));
    CHECK(back.nodes.size() == 4);

    const Compiled c = compile(again, verbs, w.find, &nodes);
    CHECK(c.problems.empty());
    const Module* door = c.find("door");
    REQUIRE(door);
    // The link and the scheme share the module; «При старте» leads nowhere.
    CHECK(door->source.find("S.on_enter = {}") == std::string::npos);
    CHECK(door->source.find("function S.on_enter(self, other)") != std::string::npos);
    CHECK(door->source.find("S.on_tick[#S.on_tick + 1] = function(self, e1)") != std::string::npos);
    CHECK(door->source.find("forge.entity.move(v3_actor, p_dt, 0)") != std::string::npos);
    CHECK(door->source.find("on_start") == std::string::npos);
    CHECK(door->touch); // the link still needs the touch
    std::string error;
    CHECK_MESSAGE(script::check_syntax(door->source, &error), error);
    // Its lines are the scheme's: after the links.
    bool mapped = false;
    for (usize i = 0; i < door->map.line_node.size(); ++i) mapped = mapped || door->map.line_node[i] == again.links.size();
    CHECK(mapped);

    // A listening thing only by its scheme, with «При входе в зону».
    Logic alone;
    ThingScheme fox;
    fox.thing = "fox";
    script::Graph fg;
    const u32 enter = fg.add("std.event.enter", 0, 0).uid;
    script::GraphNode& say = fg.add("logic.hint", 280, 0);
    say.set_value("text", "Привет");
    fg.link(enter, script::kFlowNext, say.uid);
    fox.graph = fg.to_json();
    alone.add_scheme(fox);
    const Compiled c2 = compile(alone, verbs, w.find, &nodes);
    const Module* m = c2.find("fox");
    REQUIRE(m);
    CHECK(m->touch);
    CHECK(m->source.find("S.on_enter = {}") != std::string::npos);
    CHECK(m->source.find("local hero = logic.hero()") != std::string::npos);
    CHECK_MESSAGE(script::check_syntax(m->source, &error), error);

    // «Когда» belongs to links; a broken node is the scheme's problem.
    script::Graph bad;
    bad.add("logic.when", 0, 0);
    const u32 start = bad.add("std.event.start", 0, 190).uid;
    const u32 hint = bad.add("logic.hint", 280, 190).uid; // no text
    bad.link(start, script::kFlowNext, hint);
    alone.schemes[0].graph = bad.to_json();
    const Compiled c3 = compile(alone, verbs, w.find, &nodes);
    CHECK_FALSE(c3.find("fox"));
    REQUIRE(c3.problems.size() >= 2);
    CHECK(c3.problems[0].link == alone.schemes[0].id);
    bool node_said = false;
    for (const Problem& p : c3.problems) node_said = node_said || p.node == hint;
    CHECK(node_said);
    // The whole game's code shows it as not working.
    CHECK(listing(alone, verbs, w.find, nullptr, &nodes).find("-- не работает") != std::string::npos);
}

TEST_CASE("a thing's own scheme runs by itself") {
    PoolScope pool;
    const auto dir = temp_folder("forge_logic_own");
    write_text(dir / "kinds.json", R"({"kinds": [{"id": "thing", "name": "Вещь", "group": "Разное", "icon": "box", "foot": 0.5}]})");
    write_text(dir / "objects" / "cart.object.json", R"({"id": "cart", "name": "Вагонетка", "kind": "thing"})");
    objects::Library library;
    REQUIRE(library.load(dir / "kinds.json", dir / "objects"));

    world::World world(world::WorldDesc{}, std::make_shared<EmptyGenerator>());
    scene::Scene scene(world);
    sim::Simulation sim(world, scene);
    script::ScriptHost scripts(sim, scene);
    library.attach(scene);
    NoteGame game;
    Runtime runtime(scripts, library, game);
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
    // При старте → подсказка; каждый шаг → сдвинуться на 2 клетки в секунду.
    script::Graph g = new_thing_scheme("cart");
    const u32 start = g.nodes[0].uid, tick = g.nodes[1].uid;
    script::GraphNode& hint = g.add("logic.hint", 280, 0);
    hint.set_value("text", "Поехали");
    g.link(start, script::kFlowNext, hint.uid);
    const u32 self = g.add("std.self", 0, 300).uid;
    const u32 speed = g.add("std.math.mul", 280, 300).uid;
    g.find(speed)->set_value("b", "2");
    g.link(tick, "dt", speed, "a");
    const u32 move = g.add("api.entity.move", 560, 190).uid;
    g.link(tick, script::kFlowNext, move);
    g.link(self, "actor", move, "actor");
    g.link(speed, "result", move, "dx");
    ThingScheme own;
    own.thing = "cart";
    own.graph = g.to_json();
    logic.add_scheme(own);

    std::vector<Problem> problems;
    REQUIRE(runtime.load(logic, verbs, &problems));
    for (const Problem& p : problems) INFO(p.text);
    CHECK(problems.empty());
    CHECK(runtime.link_ids() == std::vector<u32>{1});
    runtime.attach(scene);
    flecs::entity cart = library.spawn(scene, *library.find("cart"), 10.5, 6);
    REQUIRE(cart.is_valid());
    const f64 x0 = cart.get<scene::Position>().tile_x();
    run(60);
    CHECK(game.hints == std::vector<std::string>{"Поехали"});
    CHECK(cart.get<scene::Position>().tile_x() == doctest::Approx(x0 + 2).epsilon(0.05));
    CHECK_FALSE(cart.has<sim::Trigger>()); // nothing to touch
    CHECK(scripts.errors().empty());
}

namespace {

const char* kAreaVerbs = R"({
  "verbs": [
    { "id": "enter", "name": "входит в", "plural": "входят в", "icon": "login", "case": "acc",
      "when": "touch", "touch": "b", "do": "arrive", "target": "b", "a": "hero", "b": "area",
      "step": "Показать герою название места: «{b}»",
      "about": "Когда {a} входит в {b:acc}, игра показывает название места." },
    { "id": "hurt", "name": "ранит", "plural": "ранят", "case": "acc", "when": "touch", "touch": "a",
      "do": "hurt", "target": "b", "a": "thing", "b": "hero", "about": "Герой теряет сердце." }
  ]
})";

constexpr const char* kMine = "area:9f2c41d07a5be318";

} // namespace

TEST_CASE("an area is a thing the hero comes into") {
    Verbs verbs;
    REQUIRE(verbs.parse(kAreaVerbs));
    Words w;
    w.things.push_back(area_thing(kMine, "Шахта"));
    const Thing& mine = w.things.back();
    CHECK(is_area(kMine));
    CHECK_FALSE(is_area("key"));
    CHECK(mine.area);
    CHECK(mine.forms.acc == "Шахту");
    const VerbDef& enter = *verbs.find("enter");
    const VerbDef& hurt = *verbs.find("hurt");
    CHECK(suits(enter, w.things[0], mine));
    CHECK_FALSE(suits(enter, w.things[0], *w.find("key"))); // a template is not an area
    CHECK_FALSE(suits(hurt, mine, w.things[0]));            // an area is not a template
    CHECK(suits(hurt, *w.find("spikes"), w.things[0]));
    Link l{0, std::string(kHero), "enter", kMine};
    CHECK(phrase(l, enter, w.things[0], mine) == "Герой входит в Шахту");
    std::vector<Step> st = steps(l, enter, w.things[0], mine);
    REQUIRE(st.size() == 2);
    CHECK(st[0].text == "Когда герой входит в Шахту");
    CHECK(st[1].text == "Показать герою название места: «Шахта»");
    l.once = true;
    st = steps(l, enter, w.things[0], mine);
    REQUIRE(st.size() == 3);
    CHECK(st[1].refine == "once");

    // The area's module: its links when the hero comes in.
    Logic logic;
    logic.add(l);
    Compiled c = compile(logic, verbs, w.find);
    CHECK(c.problems.empty());
    REQUIRE(c.modules.size() == 1);
    CHECK(c.modules[0].thing == kMine);
    CHECK(c.modules[0].name == std::string("logic:") + kMine);
    CHECK(c.modules[0].source.find("function S.on_enter(self, other)") != std::string::npos);
    CHECK(c.modules[0].source.find("logic.first(self, 1)") != std::string::npos);
    CHECK(c.modules[0].source.find("logic.act(\"arrive\", target, \"" + std::string(kMine) + "\", hero, hero)") != std::string::npos);

    // Renamed: the same link, new words.
    w.things.back() = area_thing(kMine, "Старая штольня");
    CHECK(phrase(logic.links[0], enter, w.things[0], w.things.back()) == "Герой входит в Старую штольню");
    // Gone (deleted, or of another level): the link stays and is told about.
    w.things.pop_back();
    c = compile(logic, verbs, w.find);
    CHECK(c.modules.empty());
    REQUIRE(c.problems.size() == 1);
    CHECK(c.problems[0].text.find("нет такой зоны") != std::string::npos);
}

TEST_CASE("coming into an area runs its links; once is kept by the hero; areas are in no chunk") {
    PoolScope pool;
    const auto dir = temp_folder("forge_logic_areas");
    write_text(dir / "kinds.json", R"({"kinds": [{"id": "thing", "name": "Вещь", "group": "Разное", "icon": "box", "foot": 0.5}]})");
    objects::Library library;
    REQUIRE(library.load(dir / "kinds.json", dir / "objects"));

    world::World world(world::WorldDesc{}, std::make_shared<EmptyGenerator>());
    scene::Scene scene(world);
    sim::Simulation sim(world, scene);
    script::ScriptHost scripts(sim, scene);
    library.attach(scene);
    NoteGame game;
    Runtime runtime(scripts, library, game);
    const world::Rect view{-64, -64, 128, 64};
    auto run = [&](u32 ticks) {
        for (u32 i = 0; i < ticks; ++i) sim.update(1.0 / 60.0, view);
    };
    sim.update(0, view);
    world.finish_loading();
    run(1);

    Verbs verbs;
    REQUIRE(verbs.parse(kAreaVerbs));
    Logic logic;
    const u32 plain = logic.add({0, std::string(kHero), "enter", kMine});
    runtime.set_areas({area_thing(kMine, "Шахта"), area_thing("area:0000000000000002", "Двор")});
    std::vector<Problem> problems;
    REQUIRE(runtime.load(logic, verbs, &problems));
    CHECK(problems.empty());
    runtime.attach(scene);
    const flecs::entity_t mine = runtime.area_entity(kMine);
    REQUIRE(mine != 0);
    CHECK(runtime.is_area_entity(mine));
    CHECK(runtime.area_entity("area:0000000000000002") == 0); // no link listens to the yard
    CHECK_FALSE(scene.ecs().entity(mine).has<scene::Position>());

    flecs::entity hero = scene.spawn(scene::Position::at_tile(20, 5.5));
    game.hero_e = hero.id();
    run(2);
    CHECK(game.acts.empty());
    runtime.area_event(kMine, hero.id(), true);
    run(1);
    REQUIRE(game.acts.size() == 1);
    CHECK(game.acts[0] == std::string("arrive ") + kMine);
    CHECK(game.last_target == mine);
    CHECK(game.fired_links == std::vector<u32>{plain});
    run(5); // staying in: nothing again
    CHECK(game.acts.size() == 1);
    runtime.area_event(kMine, hero.id(), false); // leaving: no link for it
    run(1);
    CHECK(game.acts.size() == 1);
    runtime.area_event(kMine, hero.id(), true); // in again: again
    run(1);
    CHECK(game.acts.size() == 2);

    // Only once: the hero keeps it (saved with him), the area's entity keeps nothing.
    logic.find(plain)->once = true;
    REQUIRE(runtime.load(logic, verbs));
    CHECK(runtime.area_entity(kMine) == mine); // the same entity: no second one
    game.acts.clear();
    for (int i = 0; i < 3; ++i) {
        runtime.area_event(kMine, hero.id(), true);
        run(1);
        runtime.area_event(kMine, hero.id(), false);
        run(1);
    }
    CHECK(game.acts.size() == 1);
    const script::ScriptVars* vars = hero.try_get<script::ScriptVars>();
    REQUIRE(vars);
    CHECK(vars->find("связь " + std::to_string(plain)) != nullptr);
    CHECK_FALSE(scene.ecs().entity(mine).has<script::ScriptVars>());

    // Its own code that waits: an area is in no chunk, it waits in the world's time.
    logic.find(plain)->once = false;
    logic.find(plain)->code = "forge.wait(0.1)\nlogic.hint(hero, \"после\")";
    REQUIRE(runtime.load(logic, verbs));
    runtime.area_event(kMine, hero.id(), true);
    run(2);
    CHECK(game.hints.empty());
    run(10);
    CHECK(game.hints == std::vector<std::string>{"после"});
    CHECK(scripts.errors().empty());

    // The area goes: its entity goes, its link is told about and does nothing.
    runtime.set_areas({area_thing("area:0000000000000002", "Двор")});
    REQUIRE(runtime.load(logic, verbs, &problems));
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].text.find("нет такой зоны") != std::string::npos);
    CHECK(runtime.area_entity(kMine) == 0);
    CHECK_FALSE(scene.ecs().is_alive(mine));
    runtime.area_event(kMine, hero.id(), true);
    run(1);
    CHECK(game.hints.size() == 1);
    // The area is another level's of the game (step 14.2a): the link is no broken link, but nothing runs it here.
    runtime.set_other_areas({area_thing(kMine, "Шахта (Пещера)")});
    REQUIRE(runtime.load(logic, verbs, &problems));
    CHECK(problems.empty());
    CHECK(runtime.area_entity(kMine) == 0);
    runtime.area_event(kMine, hero.id(), true);
    run(12);
    CHECK(game.hints.size() == 1);
    runtime.set_other_areas({});
    // Back (an undo in the editor): it works again.
    runtime.set_areas({area_thing(kMine, "Шахта")});
    REQUIRE(runtime.load(logic, verbs, &problems));
    CHECK(problems.empty());
    runtime.area_event(kMine, hero.id(), true);
    run(12);
    CHECK(game.hints.size() == 2);
}

namespace {

const char* kGoVerbs = R"({
  "verbs": [
    { "id": "enter", "name": "входит в", "plural": "входят в", "icon": "login", "case": "acc",
      "when": "touch", "touch": "b", "do": "arrive", "target": "b", "a": "hero", "b": "area",
      "step": "Показать герою название места: «{b}»", "about": "Название места." },
    { "id": "go", "name": "уходит через", "plural": "уходят через", "icon": "door_open", "case": "acc",
      "when": "touch", "touch": "b", "do": "go", "target": "a", "a": "hero", "b": "area",
      "step": "Перейти {where}",
      "about": "Когда {a} входит в {b:acc}, он уходит {where}." }
  ]
})";

constexpr const char* kExit = "area:00000000000000e1";   // on level «Уровень 1» (id "level")
constexpr const char* kEntry = "area:00000000000000c1";  // on «Пещера» (id "cave")

struct GoWords : Words {
    GoWords() {
        things.push_back(area_thing(kExit, "Выход", "level"));
        things.push_back(area_thing(kEntry, "Вход (Пещера)", "cave"));
        things.push_back(level_thing("level", "Уровень 1"));
        things.push_back(level_thing("cave", "Пещера"));
    }
};

struct GoGame final : Game {
    flecs::entity_t hero_e = 0;
    std::vector<std::string> gos;
    bool is_hero(flecs::entity_t e) override { return e && e == hero_e; }
    flecs::entity_t hero() override { return hero_e; }
    bool has(flecs::entity_t, std::string_view) override { return false; }
    bool act(std::string_view, flecs::entity_t, std::string_view, flecs::entity_t, flecs::entity_t) override { return true; }
    void hint(flecs::entity_t, std::string_view) override {}
    void sound(flecs::entity_t, std::string_view) override {}
    bool night() override { return false; }
    Runtime* drop = nullptr; // the game drops the going at once (another one of the tick won)
    bool go(flecs::entity_t hero, std::string_view level, std::string_view arrive, u32 link) override {
        gos.push_back((hero == hero_e ? "hero " : "? ") + std::string(level) + " " + std::string(arrive) + " " + std::to_string(link));
        if (drop) drop->take_back_first(link);
        return true;
    }
};

} // namespace

TEST_CASE("a link sends the hero to a level: where to, in words, in code, in the file") {
    Verbs verbs;
    REQUIRE(verbs.parse(kGoVerbs));
    GoWords w;
    const Thing& hero = *w.find(kHero);
    const Thing& exit = *w.find(kExit);
    const Thing& cave = *w.find("level:cave");
    const VerbDef& go = *verbs.find("go");
    const VerbDef& enter = *verbs.find("enter");
    CHECK(is_level("level:cave"));
    CHECK_FALSE(is_level(kExit));
    CHECK(cave.level == "cave");
    CHECK(exit.level == "level");
    // A level is where a link leads, never one of its sides.
    CHECK(suits(go, hero, exit));
    CHECK_FALSE(suits(go, hero, cave));
    CHECK_FALSE(suits(enter, hero, cave));

    Link l{0, std::string(kHero), "go", kExit};
    CHECK(destination(l, go, w.find).empty()); // no level chosen yet
    CHECK(destination(l, enter, w.find).empty());
    l.level = "cave";
    CHECK(destination(l, go, w.find) == "на уровень «Пещера»");
    l.arrive = kEntry;
    // Another level's area is «Вход (Пещера)» among the things: the level is said once.
    const std::string where = destination(l, go, w.find);
    CHECK(where == "на уровень «Пещера», в зону «Вход»");
    CHECK(phrase(l, go, hero, exit, where) == "Герой уходит через Выход на уровень «Пещера», в зону «Вход»");
    CHECK(meaning(l, go, hero, exit, where) == "Когда герой входит в Выход, он уходит на уровень «Пещера», в зону «Вход».");
    std::vector<Step> st = steps(l, go, hero, exit, where);
    REQUIRE(st.size() == 2);
    CHECK(st[0].text == "Когда герой входит в Выход");
    CHECK(st[1].text == "Перейти на уровень «Пещера», в зону «Вход»");
    CHECK(steps(l, go, hero, exit)[1].text == "Перейти на другой уровень");

    // The exit's module asks the game to go, with the link's id.
    Logic logic;
    const u32 id = logic.add(l);
    Compiled c = compile(logic, verbs, w.find);
    for (const Problem& p : c.problems) INFO(p.text);
    CHECK(c.problems.empty());
    REQUIRE(c.modules.size() == 1);
    CHECK(c.modules[0].thing == kExit);
    CHECK(c.modules[0].source.find("logic.go(target, \"cave\", \"" + std::string(kEntry) + "\", " + std::to_string(id) + ")") !=
          std::string::npos);
    CHECK(c.modules[0].source.find("logic.act(") == std::string::npos);
    CHECK(c.modules[0].source.find("-- Герой уходит через Выход на уровень «Пещера», в зону «Вход»") != std::string::npos);
    CHECK(default_code(logic.links[0], verbs, w.find).find("logic.go(target, \"cave\"") != std::string::npos);

    // In logic.json: only when set, read back the same; a file without them reads as before.
    const std::string json = logic.json();
    CHECK(json.find("\"level\"") != std::string::npos);
    CHECK(json.find("\"arrive\"") != std::string::npos);
    Logic again;
    REQUIRE(again.parse(json));
    CHECK(again.links[0].level == "cave");
    CHECK(again.links[0].arrive == kEntry);
    CHECK(again.json() == json);
    Logic plain;
    plain.add({0, std::string(kHero), "enter", kExit});
    CHECK(plain.json().find("\"level\"") == std::string::npos);
    CHECK(plain.json().find("\"arrive\"") == std::string::npos);

    // What does not work, and why.
    auto problem = [&](const Link& with) {
        Logic one;
        one.add(with);
        const Compiled r = compile(one, verbs, w.find);
        CHECK(r.modules.empty());
        return r.problems.size() == 1 ? r.problems[0].text : std::string("?");
    };
    Link none = l;
    none.level.clear();
    none.arrive.clear();
    CHECK(problem(none) == "не выбран уровень, куда уходить");
    Link unknown = l;
    unknown.level = "l00c0ffee"; // not in the list as read: removed, or an entry levels.json could not give
    CHECK(problem(unknown).find("нет в списке уровней игры") != std::string::npos);
    Link gone = l;
    gone.arrive = "area:00000000000000ff";
    CHECK(problem(gone).find("нет зоны, где появиться") != std::string::npos);
    Link other = l;
    other.arrive = kExit; // an area of level 1, not of the cave
    CHECK(problem(other).find("не с уровня «Пещера»") != std::string::npos);
    Link spawn = l;
    spawn.arrive.clear(); // the cave's spawn point
    {
        Logic one;
        one.add(spawn);
        CHECK(compile(one, verbs, w.find).problems.empty());
    }
    // Its own code decides instead: where it leads is not asked for.
    Link coded = none;
    coded.code = "logic.hint(hero, \"нет\")\n";
    {
        Logic one;
        one.add(coded);
        CHECK(compile(one, verbs, w.find).problems.empty());
    }
}

TEST_CASE("a link that sends the hero elsewhere as a scheme: its node and the side panel are one") {
    Verbs verbs;
    REQUIRE(verbs.parse(kGoVerbs));
    GoWords w;
    script::ScriptApi api;
    script::register_core_api(api);
    const script::NodeLibrary nodes = node_library(api);
    REQUIRE(nodes.find("logic.go"));
    auto go_node = [](const script::Graph& g) -> const script::GraphNode* {
        for (const script::GraphNode& n : g.nodes)
            if (n.def == "logic.go") return &n;
        return nullptr;
    };

    Logic logic;
    Link made{0, std::string(kHero), "go", kExit};
    made.level = "cave";
    const u32 id = logic.add(made);
    Link& l = *logic.find(id);
    script::Graph g = scheme_of(l, verbs, w.find);
    const script::GraphNode* n = go_node(g);
    REQUIRE(n);
    CHECK(*n->value("level") == "cave");
    CHECK((n->value("arrive") == nullptr || n->value("arrive")->empty()));
    // Moved nodes: the link stays plain.
    for (script::GraphNode& m : g.nodes) m.x += 40;
    set_scheme(l, g, verbs, w.find);
    CHECK_FALSE(own_scheme(l, verbs, w.find));
    // Chosen in the side panel: the kept scheme follows, still plain.
    set_destination(l, "cave", kEntry);
    CHECK(l.arrive == kEntry);
    CHECK_FALSE(own_scheme(l, verbs, w.find));
    script::Graph now = scheme_of(l, verbs, w.find);
    REQUIRE(go_node(now));
    CHECK(*go_node(now)->value("arrive") == kEntry);
    CHECK(now.find(when_node(now))->x == 40); // the places kept
    // Chosen in the node: the link follows (the side panel shows it).
    for (script::GraphNode& m : now.nodes)
        if (m.def == "logic.go") {
            m.set_value("level", "level");
            m.set_value("arrive", "");
        }
    set_scheme(l, now, verbs, w.find);
    CHECK(l.level == "level");
    CHECK(l.arrive.empty());
    CHECK_FALSE(own_scheme(l, verbs, w.find));
    // The scheme compiles to the same request, with the link's id.
    l.arrive = kExit;
    set_destination(l, "level", kExit);
    script::Graph own = scheme_of(l, verbs, w.find);
    const u32 hint = own.add("logic.hint", 900, 0).uid;
    own.find(hint)->set_value("text", "Прощай");
    own.link(go_node(own)->uid, script::kFlowNext, hint);
    set_scheme(l, own, verbs, w.find);
    CHECK(own_scheme(l, verbs, w.find));
    const Compiled c = compile(logic, verbs, w.find, &nodes);
    for (const Problem& p : c.problems) INFO(p.text);
    CHECK(c.problems.empty());
    REQUIRE(c.modules.size() == 1);
    CHECK(c.modules[0].source.find("logic.go(") != std::string::npos);
    CHECK(c.modules[0].source.find(", " + std::to_string(id) + ")") != std::string::npos);
    CHECK(c.modules[0].source.find("\"Прощай\"") != std::string::npos);
}

TEST_CASE("coming into an exit asks the game to go; the game decides") {
    PoolScope pool;
    const auto dir = temp_folder("forge_logic_go");
    write_text(dir / "kinds.json", R"({"kinds": [{"id": "thing", "name": "Вещь", "group": "Разное", "icon": "box", "foot": 0.5}]})");
    objects::Library library;
    REQUIRE(library.load(dir / "kinds.json", dir / "objects"));
    world::World world(world::WorldDesc{}, std::make_shared<EmptyGenerator>());
    scene::Scene scene(world);
    sim::Simulation sim(world, scene);
    script::ScriptHost scripts(sim, scene);
    library.attach(scene);
    GoGame game;
    Runtime runtime(scripts, library, game);
    const world::Rect view{-64, -64, 128, 64};
    auto run = [&](u32 ticks) {
        for (u32 i = 0; i < ticks; ++i) sim.update(1.0 / 60.0, view);
    };
    sim.update(0, view);
    world.finish_loading();
    run(1);

    Verbs verbs;
    REQUIRE(verbs.parse(kGoVerbs));
    Logic logic;
    Link l{0, std::string(kHero), "go", kExit};
    l.level = "cave";
    l.arrive = kEntry;
    l.once = true;
    const u32 id = logic.add(l);
    runtime.set_areas({area_thing(kExit, "Выход", "level")});
    runtime.set_other_areas({area_thing(kEntry, "Вход (Пещера)", "cave")});
    runtime.set_levels({level_thing("level", "Уровень 1"), level_thing("cave", "Пещера")});
    std::vector<Problem> problems;
    REQUIRE(runtime.load(logic, verbs, &problems));
    CHECK(problems.empty());
    runtime.attach(scene);
    flecs::entity hero = scene.spawn(scene::Position::at_tile(20, 5.5));
    game.hero_e = hero.id();
    runtime.area_event(kExit, hero.id(), true);
    run(1);
    REQUIRE(game.gos.size() == 1);
    CHECK(game.gos[0] == "hero cave " + std::string(kEntry) + " " + std::to_string(id));
    // The game did not go (it refused, or another going of the tick won): the mark is taken back, the hero has
    // nothing of it, and the next time in the link asks again.
    const std::string mark = "связь " + std::to_string(id);
    CHECK((hero.try_get<script::ScriptVars>() && hero.try_get<script::ScriptVars>()->find(mark)));
    CHECK(runtime.take_back_first(id));
    CHECK_FALSE(hero.has<script::ScriptVars>());
    CHECK_FALSE(runtime.take_back_first(id));
    runtime.area_event(kExit, hero.id(), false);
    run(1);
    runtime.area_event(kExit, hero.id(), true);
    run(1);
    REQUIRE(game.gos.size() == 2);
    // A hero with variables of its own keeps them as they were.
    hero.get_mut<script::ScriptVars>().get_or_add("ключ").x = 3;
    hero.get_mut<script::ScriptVars>().get_or_add("ключ").kind = script::VarKind::Number;
    CHECK(runtime.take_back_first(id));
    REQUIRE(hero.try_get<script::ScriptVars>());
    CHECK_FALSE(hero.try_get<script::ScriptVars>()->find(mark));
    CHECK(hero.try_get<script::ScriptVars>()->find("ключ")->x == 3);
    runtime.area_event(kExit, hero.id(), false);
    run(1);
    runtime.area_event(kExit, hero.id(), true);
    run(1);
    REQUIRE(game.gos.size() == 3);
    // It went: the marks of the frame stay.
    runtime.keep_firsts();
    CHECK_FALSE(runtime.take_back_first(id));
    // Only once: the hero keeps it, as for any area.
    runtime.area_event(kExit, hero.id(), false);
    run(1);
    runtime.area_event(kExit, hero.id(), true);
    run(1);
    CHECK(game.gos.size() == 3);
    // The list without the cave (levels.json changed, or not all of it read): the link does not work.
    runtime.set_levels({level_thing("level", "Уровень 1")});
    REQUIRE(runtime.load(logic, verbs, &problems));
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].text.find("нет в списке уровней игры") != std::string::npos);
    // Its own code may ask for anything: the game checks it (a level not in its list does not go).
    logic.find(id)->once = false;
    logic.find(id)->code = "logic.go(hero, \"nowhere\", \"\", 7)\n";
    REQUIRE(runtime.load(logic, verbs, &problems));
    CHECK(problems.empty());
    runtime.area_event(kExit, hero.id(), false);
    run(1);
    runtime.area_event(kExit, hero.id(), true);
    run(1);
    REQUIRE(game.gos.size() == 4);
    CHECK(game.gos[3] == "hero nowhere  7");
    // A going dropped while the tick runs (another one won): the game takes the mark back in its go(); a hero that
    // had no variables has none again, and the next time in the link asks again.
    logic.find(id)->once = true;
    logic.find(id)->code.clear();
    runtime.set_levels({level_thing("level", "Уровень 1"), level_thing("cave", "Пещера")});
    REQUIRE(runtime.load(logic, verbs, &problems));
    CHECK(problems.empty());
    flecs::entity other = scene.spawn(scene::Position::at_tile(24, 5.5));
    game.hero_e = other.id();
    game.drop = &runtime;
    runtime.area_event(kExit, other.id(), true);
    run(1);
    REQUIRE(game.gos.size() == 5);
    CHECK_FALSE(other.has<script::ScriptVars>());
    game.drop = nullptr;
    runtime.area_event(kExit, other.id(), false);
    run(1);
    runtime.area_event(kExit, other.id(), true);
    run(1);
    REQUIRE(game.gos.size() == 6);
    runtime.keep_firsts();
    CHECK((other.try_get<script::ScriptVars>() && other.try_get<script::ScriptVars>()->find(mark)));
    CHECK(scripts.errors().empty());
}

// The rest of a frame's ticks do not run once a game leaves the world in one (the hero goes to another level).
TEST_CASE("a simulation stops after the tick it is told to") {
    PoolScope pool;
    world::World world(world::WorldDesc{}, std::make_shared<EmptyGenerator>());
    scene::Scene scene(world);
    sim::SimDesc desc;
    sim::Simulation sim(world, scene, desc);
    const world::Rect view{-8, -8, 8, 8};
    sim.update(0, view);
    world.finish_loading();
    u32 seen = 0;
    bool stop = false;
    sim.add_system([&](const sim::TickContext&) {
        ++seen;
        if (stop) sim.stop_ticks();
    });
    const f64 three = 3.5 / static_cast<f64>(desc.ticks_per_second);
    const u32 first = sim.update(three, view);
    CHECK(first >= 2);
    CHECK(seen == first);
    stop = true;
    CHECK(sim.update(three, view) == 1);
    CHECK(seen == first + 1);
    stop = false;
    const u32 next = sim.update(three, view); // the next frame runs as before
    CHECK(next >= 2);
    CHECK(seen == first + 1 + next);
}

// Files that are not there, named in Cyrillic: the error names them, nothing throws.
TEST_CASE("logic: verbs, ideas and links not there, their names in Cyrillic") {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / utf8_path("forge_tests_связи нет");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::string error;
    Verbs verbs;
    CHECK_FALSE(verbs.load(dir / utf8_path("глаголы.json"), &error));
    CHECK(error == "не читается глаголы.json");
    // A file where its folder should be.
    const std::string text = "файл";
    REQUIRE(write_file_atomic(dir / utf8_path("занято"), std::span(reinterpret_cast<const u8*>(text.data()), text.size())));
    Logic links;
    error.clear();
    CHECK_FALSE(links.save(dir / utf8_path("занято") / utf8_path("связи.json"), &error));
    CHECK(error == "не записывается связи.json");
}
