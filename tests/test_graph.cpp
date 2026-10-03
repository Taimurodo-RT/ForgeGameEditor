#include "forge/core/file.h"
#include "forge/core/jobs.h"
#include "forge/core/path.h"
#include "forge/script/compiler.h"
#include "forge/script/graph.h"
#include "forge/script/host.h"
#include "forge/script/nodes.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <memory>

// The demo's look component, the same fields under the same name.
struct DemoLook {
    forge::u32 frame = 0;
    forge::f32 r = 1, g = 1, b = 1;
};
FORGE_REFLECT_DECLARE(DemoLook)
FORGE_REFLECT(DemoLook, 1) {
    t.field("frame", &DemoLook::frame);
    t.field("r", &DemoLook::r);
    t.field("g", &DemoLook::g);
    t.field("b", &DemoLook::b);
}

using namespace forge;
using namespace forge::world;
using namespace forge::sim;
using namespace forge::script;
using scene::Position;

namespace {

struct GraphPool {
    GraphPool() { jobs::init(3); }
    ~GraphPool() { jobs::shutdown(); }
};

class FlatGenerator final : public Generator {
public:
    void generate(ChunkCoord, const ChunkTiles& out) const override {
        for (u32 l = 0; l < out.layer_count; ++l)
            for (u32 i = 0; i < kChunkTiles; ++i) out.layer(l)[i] = 0;
    }
};

// A small world with scripts and the full node library.
struct Stage {
    World world;
    scene::Scene scene;
    Simulation sim;
    ScriptHost scripts;
    NodeLibrary lib;
    Rect view{-64, -64, 128, 64};

    Stage() : world(WorldDesc{}, std::make_shared<FlatGenerator>()), scene(world), sim(world, scene), scripts(sim, scene) {
        sim.update(0, view);
        world.finish_loading();
        sim.update(0, view);
        std::vector<std::string> errors;
        REQUIRE(lib.add_standard(&errors));
        REQUIRE(errors.empty());
        lib.add_api(scripts.api());
        lib.add_components(scripts.exposed_types());
    }
    // Compiles and loads; fails the test with the diagnostics when it does not compile.
    CompileResult load(const Graph& g, CompileOptions opt = {}) {
        CompileResult r = compile(g, lib, opt);
        for (const Diagnostic& d : r.diagnostics) MESSAGE(d.node, ": ", d.message);
        if (!r.ok) MESSAGE(r.source);
        REQUIRE(r.ok);
        std::vector<ScriptError> errs;
        const bool loaded = scripts.load(g.name, r.source, &r.map, &errs);
        if (!loaded) {
            MESSAGE(r.source);
            for (const ScriptError& e : errs) MESSAGE(e.line, ": ", e.message);
        }
        REQUIRE(loaded);
        return r;
    }
    flecs::entity thing(const std::string& script, f64 x = 3, f64 y = 3) {
        flecs::entity e = scene.spawn(Position::at_tile(x, y));
        e.set<Script>({script});
        return e;
    }
    void run(u32 ticks) {
        for (u32 i = 0; i < ticks; ++i) sim.update(1.0 / 60.0, view);
    }
    f64 var(flecs::entity e, const char* name) {
        const ScriptVars* v = e.try_get<ScriptVars>();
        const ScriptVar* s = v ? v->find(name) : nullptr;
        return s ? s->x : -1;
    }
};

// "Add <amount> to variable <name>" as one block.
u32 add_to(Graph& g, const char* name, const char* amount = "1") {
    GraphNode& n = g.add("std.var.add");
    n.set_value("name", name);
    n.set_value("amount", amount);
    return n.uid;
}

u32 set_var(Graph& g, const char* name, const char* value) {
    GraphNode& n = g.add("std.var.set");
    n.set_value("name", name);
    n.set_value("value", value);
    return n.uid;
}

bool has_error(const CompileResult& r, u32 node, std::string_view text) {
    return std::any_of(r.diagnostics.begin(), r.diagnostics.end(), [&](const Diagnostic& d) {
        return d.error && d.node == node && d.message.find(text) != std::string::npos;
    });
}

} // namespace

TEST_CASE("the standard node library is consistent") {
    NodeLibrary lib;
    std::vector<std::string> errors;
    REQUIRE(lib.add_standard(&errors));
    CHECK(errors.empty());
    CHECK(lib.all().size() >= 40);
    for (const NodeDef& d : lib.all()) {
        CAPTURE(d.id);
        CHECK(!d.title.get().empty());
        CHECK(!d.category.empty());
        CHECK(d.module == "std");
        if (d.kind == NodeKind::Event && d.id != "std.graph.entry") CHECK(!d.event.empty());
        // Every placeholder in a template names a pin or slot the node has.
        const std::string& t = d.lua;
        for (usize i = t.find('{'); i != std::string::npos; i = t.find('{', i + 1)) {
            const usize close = t.find('}', i);
            REQUIRE(close != std::string::npos);
            const std::string ph = t.substr(i + 1, close - i - 1);
            if (ph.rfind("in:", 0) == 0) CHECK_MESSAGE(d.input(ph.substr(3)), ph);
            if (ph.rfind("out:", 0) == 0) CHECK_MESSAGE(d.output(ph.substr(4)), ph);
            if (ph.rfind("slot:", 0) == 0) CHECK_MESSAGE(d.slot(ph.substr(5)), ph);
        }
    }
    // A broken file says what is wrong and keeps what was there.
    CHECK_FALSE(lib.add_json(R"J({"nodes": [{"id": "x.bad", "inputs": [{"id": "in"}]}]})J", "mod", &errors));
    CHECK(lib.find("x.bad") == nullptr);
    CHECK_FALSE(lib.add_json("{nodes", "mod", &errors));
    CHECK(lib.find("std.flow.if"));
    // A module can override a standard node by id.
    REQUIRE(lib.add_json(R"J({"module": "mine", "nodes": [{"id": "std.math.abs", "kind": "pure", "title": {"ru": "Модуль"},
        "inputs": [{"id": "a", "type": "number"}], "outputs": [{"id": "result", "type": "number"}], "lua": "math.abs({in:a})"}]})J",
                         "mine.nodes.json"));
    CHECK(lib.find("std.math.abs")->title.get() == "Модуль");
    CHECK(lib.find("std.math.abs")->module == "mine");
}

TEST_CASE("engine functions and component fields become nodes") {
    GraphPool pool;
    Stage s;
    const NodeDef* destroy = s.lib.find("api.entity.destroy");
    REQUIRE(destroy);
    CHECK(destroy->kind == NodeKind::Action);
    CHECK(destroy->call == "entity.destroy");
    REQUIRE(destroy->input("actor"));
    CHECK(destroy->input("actor")->value == "self");
    const NodeDef* wait = s.lib.find("api.wait");
    REQUIRE(wait);
    CHECK(wait->latent);
    CHECK(wait->input("seconds")->value == "1");
    CHECK(s.lib.find("api.prof.enter") == nullptr); // hidden ones stay out of the palette
    CHECK(s.lib.find("api.compare") == nullptr);

    const NodeDef* vx = s.lib.find("comp.Body.vx.get");
    REQUIRE(vx);
    CHECK(vx->kind == NodeKind::Pure);
    CHECK(vx->output("value")->type == ValueType::Number);
    REQUIRE(s.lib.find("comp.Body.vx.set"));
    CHECK(s.lib.find("comp.Body.vx.set")->input("value")->required);
}

TEST_CASE("a door graph compiles and runs") {
    GraphPool pool;
    Stage s;
    // When the game starts: state = 1, wait a second, state = 2.
    Graph g;
    g.name = "door";
    const u32 start = g.add("std.event.start").uid;
    const u32 open = set_var(g, "state", "1");
    GraphNode& w = g.add("api.wait");
    w.set_value("seconds", "0.5");
    const u32 wait = w.uid;
    const u32 shut = set_var(g, "state", "2");
    g.link(start, kFlowNext, open);
    g.link(open, kFlowNext, wait);
    g.link(wait, kFlowNext, shut);
    // Every tick: ticks += 1; when ticks >= 10, done = 1.
    const u32 tick = g.add("std.event.tick").uid;
    const u32 count = add_to(g, "ticks");
    const u32 get = g.add("std.var.get").uid;
    g.find(get)->set_value("name", "ticks");
    GraphNode& cmp = g.add("std.compare");
    cmp.set_value("op", "≥");
    cmp.set_value("b", "10");
    const u32 compare = cmp.uid;
    const u32 branch = g.add("std.flow.if").uid;
    const u32 done = set_var(g, "done", "1");
    g.link(tick, kFlowNext, count);
    g.link(count, kFlowNext, branch);
    g.link(get, "value", compare, "a");
    g.link(compare, "result", branch, "cond");
    g.put_in_slot(branch, "then", done);
    g.variables.push_back({"ticks", ValueType::Number, "0", {}});

    const CompileResult r = s.load(g);
    CHECK(r.diagnostics.empty());
    // Lines know their nodes.
    CHECK(std::count(r.map.line_node.begin(), r.map.line_node.end(), branch) > 0);
    CHECK(std::count(r.map.line_node.begin(), r.map.line_node.end(), compare) > 0);

    flecs::entity door = s.thing("door");
    s.run(9);
    CHECK(s.var(door, "state") == 1);
    CHECK(s.var(door, "done") == -1);
    s.run(1);
    CHECK(s.var(door, "done") == 1);
    s.run(30);
    CHECK(s.var(door, "state") == 2);
    CHECK(s.scripts.errors().empty());
}

TEST_CASE("the compiler explains what is missing or wrong, in the user's terms") {
    GraphPool pool;
    Stage s;
    Graph g;
    g.name = "bad";
    const u32 start = g.add("std.event.start").uid;
    const u32 branch = g.add("std.flow.if").uid;
    g.link(start, kFlowNext, branch);
    CompileResult r = compile(g, s.lib);
    CHECK_FALSE(r.ok);
    CHECK(has_error(r, branch, "Подключите «Условие»"));

    // A wire carrying an object into a number.
    const u32 self = g.add("std.self").uid;
    const u32 sum = g.add("std.math.add").uid;
    const u32 setter = set_var(g, "x", "");
    g.link(self, "actor", sum, "a");
    g.link(sum, "result", setter, "value");
    g.link(branch, "then", setter);
    r = compile(g, s.lib);
    CHECK(has_error(r, sum, "объект"));
    g.link(start, kFlowNext, setter); // a second wire from one exit
    r = compile(g, s.lib);
    CHECK(has_error(r, start, "только один провод"));

    // A wire into a value that must be typed, a pin that does not exist, a loop of wires.
    Graph h;
    h.name = "worse";
    const u32 e = h.add("std.event.start").uid;
    const u32 a = add_to(h, "a");
    const u32 b = add_to(h, "b");
    const u32 msg = h.add("std.event.message").uid;
    h.link(e, kFlowNext, a);
    h.link(a, kFlowNext, b);
    h.link(b, kFlowNext, a);
    r = compile(h, s.lib);
    CHECK(has_error(r, a, "в круг"));
    h.links.pop_back();
    h.link(msg, "name", e, "nope");
    r = compile(h, s.lib);
    CHECK(std::any_of(r.diagnostics.begin(), r.diagnostics.end(), [](const Diagnostic& d) { return d.message.find("нет входа") != std::string::npos; }));

    // Unconnected blocks are a warning, not an error.
    Graph w;
    w.name = "lonely";
    w.add("std.event.start");
    const u32 orphan = add_to(w, "n");
    r = compile(w, s.lib);
    CHECK(r.ok);
    REQUIRE(r.diagnostics.size() == 1);
    CHECK_FALSE(r.diagnostics[0].error);
    CHECK(r.diagnostics[0].node == orphan);
}

TEST_CASE("branches that meet again share one copy of what follows") {
    GraphPool pool;
    Stage s;
    Graph g;
    g.name = "fan";
    const u32 start = g.add("std.event.start").uid;
    const u32 branch = g.add("std.flow.if").uid;
    g.find(branch)->set_value("cond", "true");
    const u32 left = add_to(g, "left");
    const u32 right = add_to(g, "right");
    const u32 join = add_to(g, "joined");
    const u32 after = add_to(g, "after");
    g.link(start, kFlowNext, branch);
    g.link(branch, "then", left);
    g.link(branch, "else", right);
    g.link(left, kFlowNext, join);
    g.link(right, kFlowNext, join);
    g.link(join, kFlowNext, after);
    const CompileResult r = s.load(g);
    CHECK(r.source.find("c" + std::to_string(join) + " = function()") != std::string::npos);
    flecs::entity e = s.thing("fan");
    s.run(1);
    CHECK(s.var(e, "left") == 1);
    CHECK(s.var(e, "right") == -1);
    CHECK(s.var(e, "joined") == 1);
    CHECK(s.var(e, "after") == 1);
}

TEST_CASE("slots and loops") {
    GraphPool pool;
    Stage s;
    Graph g;
    g.name = "loops";
    g.variables.push_back({"w", ValueType::Number, "0", {}});
    const u32 start = g.add("std.event.start").uid;
    // Repeat 5 times: r += index; after: after = 1.
    const u32 rep = g.add("std.flow.repeat").uid;
    g.find(rep)->set_value("count", "5");
    const u32 body = add_to(g, "r", "0");
    g.link(rep, "index", body, "amount");
    g.put_in_slot(rep, "body", body);
    const u32 after = set_var(g, "after", "1");
    g.link(rep, "after", after);
    g.link(start, kFlowNext, rep);
    // While w < 4: w += 1 (the condition is checked on every pass).
    const u32 loop = g.add("std.flow.while").uid;
    const u32 get = g.add("std.var.get").uid;
    g.find(get)->set_value("name", "w");
    GraphNode& cmp = g.add("std.compare");
    cmp.set_value("op", "<");
    cmp.set_value("b", "4");
    const u32 less = cmp.uid;
    g.link(get, "value", less, "a");
    g.link(less, "result", loop, "cond");
    g.put_in_slot(loop, "body", add_to(g, "w"));
    g.link(after, kFlowNext, loop);
    // Do once, on every tick: once += 1.
    const u32 tick = g.add("std.event.tick").uid;
    const u32 once = g.add("std.flow.once").uid;
    g.put_in_slot(once, "body", add_to(g, "once"));
    g.link(tick, kFlowNext, once);
    // A sequence runs its steps in order.
    const u32 seq = g.add("std.flow.sequence").uid;
    g.put_in_slot(seq, "step1", set_var(g, "order", "1"));
    g.put_in_slot(seq, "step2", add_to(g, "order", "10"));
    g.link(loop, "after", seq);

    s.load(g);
    flecs::entity e = s.thing("loops");
    s.run(5);
    CHECK(s.var(e, "r") == 15);
    CHECK(s.var(e, "after") == 1);
    CHECK(s.var(e, "w") == 4);
    CHECK(s.var(e, "once") == 1);
    CHECK(s.var(e, "order") == 11);
    CHECK(s.scripts.errors().empty());
}

TEST_CASE("graphs can be used as nodes") {
    GraphPool pool;
    Stage s;
    // A pure graph: y = x * 2.
    Graph dbl;
    dbl.name = "double";
    dbl.as_node = {true, "my.double", NodeKind::Pure, "Мои ноды", "", {"Удвоить", "Double"}, {}};
    PinDef x;
    x.id = "x";
    x.type = ValueType::Number;
    PinDef y = x;
    y.id = "y";
    dbl.inputs = {x};
    dbl.outputs = {y};
    const u32 in = dbl.add("std.graph.entry").uid;
    const u32 mul = dbl.add("std.math.mul").uid;
    const u32 ret = dbl.add("std.graph.return").uid;
    dbl.link(in, "x", mul, "a");
    dbl.link(mul, "result", ret, "y");
    REQUIRE(s.lib.add_graph_node(dbl));

    // An action graph that uses the pure one: score += double(amount).
    Graph bump;
    bump.name = "bump";
    bump.as_node = {true, "my.bump", NodeKind::Action, {}, {}, {}, {}};
    PinDef amount = x;
    amount.id = "amount";
    bump.inputs = {amount};
    const u32 bin = bump.add("std.graph.entry").uid;
    const u32 call = bump.add("my.double").uid;
    const u32 add = add_to(bump, "score");
    bump.link(bin, "amount", call, "x");
    bump.link(call, "y", add, "amount");
    bump.link(bin, kFlowNext, add);
    REQUIRE(s.lib.add_graph_node(bump));
    CHECK(s.lib.find("my.bump")->category == "Мои ноды");

    Graph g;
    g.name = "player";
    const u32 start = g.add("std.event.start").uid;
    GraphNode& b1 = g.add("my.bump");
    b1.set_value("amount", "5");
    const u32 first = b1.uid;
    GraphNode& b2 = g.add("my.bump");
    b2.set_value("amount", "16");
    const u32 second = b2.uid;
    g.link(start, kFlowNext, first);
    g.link(first, kFlowNext, second);
    const CompileResult r = s.load(g);
    // Each graph is compiled once, however many times it is used.
    CHECK(r.source.find("local function g_double") != std::string::npos);
    CHECK(r.source.find("local function g_bump") != std::string::npos);
    CHECK(r.source.find("local function g_bump", r.source.find("local function g_bump") + 1) == std::string::npos);
    flecs::entity e = s.thing("player");
    s.run(1);
    CHECK(s.var(e, "score") == 42);

    // A graph that uses itself is refused.
    Graph loop;
    loop.name = "loop";
    loop.as_node = {true, "my.loop", NodeKind::Action, {}, {}, {}, {}};
    const u32 lin = loop.add("std.graph.entry").uid;
    loop.link(lin, kFlowNext, loop.add("my.loop").uid);
    REQUIRE(s.lib.add_graph_node(loop));
    Graph user;
    user.name = "user";
    user.link(user.add("std.event.start").uid, kFlowNext, user.add("my.loop").uid);
    CHECK_FALSE(compile(user, s.lib).ok);
}

TEST_CASE("old graphs are brought up to date by the library's migrations") {
    GraphPool pool;
    Stage s;
    REQUIRE(s.lib.add_json(R"J({"module": "test", "nodes": [
        {"id": "test.counter", "version": 3, "kind": "action", "category": "T", "title": {"ru": "Счётчик"},
         "inputs": [{"id": "name", "type": "string"}, {"id": "step", "type": "number", "value": "1"}],
         "lua": "forge.set_var(self, {in:name}, forge.var(self, {in:name}, 0) + {in:step})",
         "migrations": [
           {"from": 1, "rename_pins": [{"from": "by", "to": "amount"}]},
           {"from": 2, "rename_pins": [{"from": "amount", "to": "step"}], "set_values": [{"pin": "name", "value": "count"}]}]},
        {"id": "test.old_add", "version": 2, "kind": "action", "category": "T", "title": {"ru": "Старое"}, "hidden": true,
         "migrations": [{"from": 1, "rename_pins": [{"from": "how_much", "to": "amount"}], "replaced_by": "std.var.add"}]}
    ]})J", "test"));

    Graph g;
    g.name = "old";
    const u32 start = g.add("std.event.start").uid;
    GraphNode& c = g.add("test.counter");
    c.version = 1;
    c.set_value("by", "7");
    const u32 counter = c.uid;
    GraphNode& o = g.add("test.old_add");
    o.version = 1;
    o.set_value("name", "legacy");
    o.set_value("how_much", "3");
    const u32 old = o.uid;
    g.link(start, kFlowNext, counter);
    g.link(counter, kFlowNext, old);

    std::vector<std::string> notes;
    migrate(g, s.lib, &notes);
    CHECK(notes.size() == 3);
    CHECK(g.find(counter)->version == 3);
    REQUIRE(g.find(counter)->value("step"));
    CHECK(*g.find(counter)->value("step") == "7");
    CHECK(*g.find(counter)->value("name") == "count");
    CHECK(g.find(old)->def == "std.var.add");
    CHECK(*g.find(old)->value("amount") == "3");

    s.load(g);
    flecs::entity e = s.thing("old");
    s.run(1);
    CHECK(s.var(e, "count") == 7);
    CHECK(s.var(e, "legacy") == 3);
}

TEST_CASE("graphs survive JSON, nodes from missing modules included") {
    GraphPool pool;
    Stage s;
    Graph g;
    g.name = "saved";
    g.variables.push_back({"hp", ValueType::Number, "10", {"Здоровье", "Health"}});
    const u32 start = g.add("std.event.start", 10, 20).uid;
    GraphNode& m = g.add("mod.weather.rain", 200, 20);
    m.set_value("strength", "0.8");
    m.comment = "из модуля погоды";
    const u32 rain = m.uid;
    const u32 branch = g.add("std.flow.if").uid;
    g.put_in_slot(branch, "then", add_to(g, "hp", "-1"));
    g.link(start, kFlowNext, rain);
    g.link(rain, kFlowNext, branch);

    const std::string json = g.to_json();
    Graph back;
    std::string err;
    REQUIRE(back.from_json(json, &err));
    CHECK(back.to_json() == json);
    CHECK(back.next_uid == g.next_uid);
    REQUIRE(back.find(rain));
    CHECK(back.find(rain)->def == "mod.weather.rain");
    CHECK(*back.find(rain)->value("strength") == "0.8");
    CHECK(back.variable("hp")->title.get() == "Здоровье");

    // The missing node is reported, and stays in the graph after a migration.
    const CompileResult r = compile(back, s.lib);
    CHECK_FALSE(r.ok);
    CHECK(has_error(r, rain, "не найдена"));
    migrate(back, s.lib);
    CHECK(back.find(rain)->def == "mod.weather.rain");
    CHECK(back.to_json() == json);
    CHECK_FALSE(back.from_json("{\"nodes\": [", &err));
    CHECK(!err.empty());
}

TEST_CASE("profiled graphs report time per node and runtime errors point at nodes") {
    GraphPool pool;
    Stage s;
    Graph g;
    g.name = "prof";
    const u32 tick = g.add("std.event.tick").uid;
    const u32 add = add_to(g, "n");
    const u32 also = add_to(g, "m");
    g.link(tick, kFlowNext, add);
    g.link(add, kFlowNext, also);
    // Two start events run side by side.
    g.link(g.add("std.event.start").uid, kFlowNext, add_to(g, "starts"));
    g.link(g.add("std.event.start").uid, kFlowNext, add_to(g, "starts"));
    CompileOptions opt;
    opt.profile = true;
    s.load(g, opt);
    flecs::entity e = s.thing("prof");
    s.run(10);
    CHECK(s.var(e, "n") == 10);
    CHECK(s.var(e, "starts") == 2);
    const std::vector<NodeTime> prof = s.scripts.node_profile();
    auto found = std::find_if(prof.begin(), prof.end(), [&](const NodeTime& t) { return t.node == add; });
    REQUIRE(found != prof.end());
    CHECK(found->script == "prof");
    CHECK(found->calls == 10);
    CHECK(found->ms >= 0);

    // An operator the engine does not know fails at run time, on its node.
    Graph bad;
    bad.name = "badop";
    const u32 start = bad.add("std.event.start").uid;
    GraphNode& cmp = bad.add("std.compare");
    cmp.set_value("a", "1");
    cmp.set_value("op", "<=>");
    const u32 compare = cmp.uid;
    const u32 setter = set_var(bad, "x", "");
    bad.link(compare, "result", setter, "value");
    bad.link(start, kFlowNext, setter);
    s.load(bad);
    s.thing("badop");
    s.run(1);
    REQUIRE(s.scripts.errors().size() == 1);
    CHECK(s.scripts.errors()[0].node == compare);
    CHECK(s.scripts.errors()[0].message.find("<=>") != std::string::npos);
}

TEST_CASE("the demo's graphs compile and work") {
    GraphPool pool;
    Stage s;
    s.scene.register_component<DemoLook>();
    s.scripts.expose<DemoLook>();
    s.lib.add_components(s.scripts.exposed_types());
    std::vector<Graph> graphs;
    for (const char* name : {"door_column", "door", "chest", "coin", "critter"}) {
        CAPTURE(name);
        std::vector<u8> bytes;
        REQUIRE(read_file(utf8_path(std::string(FORGE_SOURCE_DIR "/apps/script_demo/graphs/") + name + ".graph.json"), bytes));
        Graph g;
        std::string err;
        REQUIRE_MESSAGE(g.from_json(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), &err), err);
        CHECK(g.name == name);
        if (g.as_node.enabled) REQUIRE(s.lib.add_graph_node(g));
        else graphs.push_back(std::move(g));
    }
    for (const Graph& g : graphs) {
        CAPTURE(g.name);
        CHECK(s.load(g).diagnostics.empty());
    }

    // A floor at y = 10; the door's column is the three tiles above it.
    for (i32 x = 0; x < 40; ++x) s.world.set_tile(1, x, 10, 3);
    s.sim.collision().set(3, TileShape::Solid);
    flecs::entity door = s.thing("door", 5.5, 8.5);
    door.set<Trigger>({2.5f});
    flecs::entity chest = s.thing("chest", 30.5, 9.5);
    chest.set<Trigger>({1.5f});
    chest.set<DemoLook>({29, 1, 1, 1});
    s.run(2);
    for (i32 y = 7; y <= 9; ++y) CHECK(s.world.tile(1, 5, y) == 3);

    // Someone comes near: the door opens, then closes behind them.
    flecs::entity walker = s.scene.spawn(Position::at_tile(1.5, 9.5));
    walker.set<Body>({});
    for (u32 i = 0; i < 20; ++i) {
        walker.get_mut<Position>() = Position::at_tile(1.5 + i * 0.5, 9.5);
        s.run(1);
        if (i == 6) CHECK(s.world.tile(1, 5, 8) == 0);
    }
    CHECK(s.world.tile(1, 5, 8) == 3);

    // At the chest coins fly out, once, then disappear.
    walker.get_mut<Position>() = Position::at_tile(30.5, 9.5);
    s.run(10);
    CHECK(chest.try_get<DemoLook>()->b == doctest::Approx(0.3));
    u32 coins = 0;
    s.scene.ecs().each([&](const Body& b, const DemoLook& look) {
        if (look.frame == 19) {
            ++coins;
            CHECK(b.gravity == 1);
        }
    });
    CHECK(coins == 8);
    walker.get_mut<Position>() = Position::at_tile(20.5, 9.5);
    s.run(10);
    walker.get_mut<Position>() = Position::at_tile(30.5, 9.5);
    s.run(300);
    coins = 0;
    s.scene.ecs().each([&](const DemoLook& look) { coins += look.frame == 19; });
    CHECK(coins == 0);
    CHECK(s.scripts.errors().empty());
}
