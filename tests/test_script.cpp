#include "forge/core/jobs.h"
#include "forge/script/host.h"

#include <doctest/doctest.h>

#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace forge;
using namespace forge::world;
using namespace forge::sim;
using namespace forge::script;
using scene::Position;

namespace {

struct PoolScope {
    PoolScope() { jobs::init(3); }
    ~PoolScope() { jobs::shutdown(); }
};

class EmptyGenerator final : public Generator {
public:
    void generate(ChunkCoord, const ChunkTiles& out) const override {
        for (u32 l = 0; l < out.layer_count; ++l)
            for (u32 i = 0; i < kChunkTiles; ++i) out.layer(l)[i] = 0;
    }
};

struct Room {
    World world;
    scene::Scene scene;
    Simulation sim;
    ScriptHost scripts;
    Rect view{-64, -64, 128, 64};

    explicit Room(ScriptDesc sd = {})
        : world(WorldDesc{}, std::make_shared<EmptyGenerator>()), scene(world), sim(world, scene), scripts(sim, scene, sd) {
        sim.update(0, view);
        world.finish_loading();
        sim.update(0, view);
    }
    flecs::entity thing(f64 x, f64 y, const char* script) {
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

} // namespace

TEST_CASE("modules run on_start once and on_tick every tick") {
    PoolScope pool;
    Room room;
    REQUIRE(room.scripts.load("counter", R"(
        local S = {}
        function S.on_start(self) forge.set_var(self, "starts", forge.var(self, "starts", 0) + 1) end
        function S.on_tick(self, dt)
            forge.set_var(self, "ticks", forge.var(self, "ticks", 0) + 1)
            forge.set_var(self, "time", forge.var(self, "time", 0) + dt)
        end
        return S
    )"));
    flecs::entity a = room.thing(3, 3, "counter");
    room.run(60);
    CHECK(room.var(a, "starts") == 1);
    CHECK(room.var(a, "ticks") == 60);
    CHECK(room.var(a, "time") == doctest::Approx(1.0).epsilon(0.01));
    CHECK(room.scripts.errors().empty());
}

TEST_CASE("scripts are sandboxed and modules keep to themselves") {
    PoolScope pool;
    Room room;
    std::string err;
    CHECK_FALSE(room.scripts.run("io.open('x')", &err));
    CHECK_FALSE(room.scripts.run("os.execute('x')", &err));
    CHECK_FALSE(room.scripts.run("forge.log = nil", &err)); // the engine's table is read-only
    REQUIRE(room.scripts.load("a", "shared = 1 return {}"));
    REQUIRE(room.scripts.load("b", "assert(shared == nil) return {}"));
}

TEST_CASE("wait follows the entity's world time") {
    PoolScope pool;
    Room room;
    REQUIRE(room.scripts.load("door", R"(
        local S = {}
        function S.on_start(self)
            forge.set_var(self, "state", 1)
            forge.wait(1)
            forge.set_var(self, "state", 2)
        end
        return S
    )"));
    flecs::entity a = room.thing(3, 3, "door");
    room.run(30);
    CHECK(room.var(a, "state") == 1);
    CHECK(room.scripts.stats().waiting == 1);
    room.run(32);
    CHECK(room.var(a, "state") == 2);
    CHECK(room.scripts.stats().waiting == 0);

    // Slow motion: the same wait takes four times as long.
    flecs::entity b = room.thing(5, 3, "door");
    room.sim.set_time_scale(0.25f);
    room.run(120);
    CHECK(room.var(b, "state") == 1);
    room.run(124);
    CHECK(room.var(b, "state") == 2);

    // Stopped time: nothing runs, except for things outside time.
    room.sim.set_time_scale(0);
    flecs::entity c = room.thing(7, 3, "door");
    flecs::entity d = room.thing(9, 3, "door");
    d.set<OutsideTime>({1.0f});
    room.run(120);
    CHECK(room.var(c, "state") == -1); // not even started
    CHECK(room.var(d, "state") == 2);
}

TEST_CASE("a runaway loop is stopped and only its entity is switched off") {
    PoolScope pool;
    ScriptDesc sd;
    sd.budget_ms = 20;
    Room room(sd);
    REQUIRE(room.scripts.load("stuck", R"(
        local S = {}
        function S.on_tick(self)
            while true do end
        end
        return S
    )"));
    REQUIRE(room.scripts.load("fine", R"(
        local S = {}
        function S.on_tick(self) forge.set_var(self, "n", forge.var(self, "n", 0) + 1) end
        return S
    )"));
    flecs::entity bad = room.thing(3, 3, "stuck");
    flecs::entity good = room.thing(5, 3, "fine");
    room.run(10);
    REQUIRE(room.scripts.errors().size() == 1);
    const ScriptError& e = room.scripts.errors()[0];
    CHECK(e.script == "stuck");
    CHECK(e.handler == "on_tick");
    CHECK(e.line == 4);
    CHECK(e.entity == bad.id());
    CHECK(bad.try_get<Script>()->failed);
    CHECK(room.var(good, "n") == 10);
}

TEST_CASE("errors point at the line and, for graphs, the node") {
    PoolScope pool;
    Room room;
    SourceMap map;
    map.line_node = {SourceMap::kNoNode, 7, 7, 9, SourceMap::kNoNode};
    REQUIRE(room.scripts.load("graph", "local S = {}\nfunction S.on_start(self)\nlocal x = 1\nlocal y = nil + x\nend\nreturn S", &map));
    room.thing(3, 3, "graph");
    room.run(1);
    REQUIRE(room.scripts.errors().size() == 1);
    CHECK(room.scripts.errors()[0].line == 4);
    CHECK(room.scripts.errors()[0].node == 9);

    std::vector<ScriptError> errs;
    CHECK_FALSE(room.scripts.load("broken", "return {", nullptr, &errs));
    REQUIRE(errs.size() == 1);
    CHECK(errs[0].handler == "load");
    CHECK(errs[0].line == 1);
}

TEST_CASE("reloading a module keeps the game state") {
    PoolScope pool;
    Room room;
    REQUIRE(room.scripts.load("m", "return { on_tick = function(self) forge.set_var(self, 'n', forge.var(self, 'n', 0) + 1) end }"));
    flecs::entity a = room.thing(3, 3, "m");
    room.run(5);
    CHECK(room.var(a, "n") == 5);
    REQUIRE(room.scripts.load("m", "return { on_tick = function(self) forge.set_var(self, 'n', forge.var(self, 'n', 0) + 100) end }"));
    room.run(1);
    CHECK(room.var(a, "n") == 105);
    // A broken new version leaves the old one running.
    CHECK_FALSE(room.scripts.load("m", "return { on_tick = function(self) +++ end }"));
    room.run(1);
    CHECK(room.var(a, "n") == 205);
}

TEST_CASE("components through reflection, triggers and messages") {
    PoolScope pool;
    Room room;
    REQUIRE(room.scripts.load("pad", R"(
        local S = {}
        function S.on_enter(self, other)
            forge.set(other, "Body", "vy", -20)
            forge.send(other, "launched", 20)
        end
        return S
    )"));
    REQUIRE(room.scripts.load("ball", R"(
        local S = {}
        function S.on_message(self, name, value, from)
            if name == "launched" then forge.set_var(self, "got", value) end
        end
        return S
    )"));
    flecs::entity pad = room.thing(10, 5, "pad");
    pad.set<Trigger>({1.5f});
    flecs::entity ball = room.thing(4, 5, "ball");
    Body b;
    b.vx = 6;
    b.gravity = 0;
    ball.set<Body>(b);
    room.run(120);
    CHECK(room.var(ball, "got") == 20);
    CHECK(ball.try_get<Body>()->vy == doctest::Approx(-20));
    CHECK(room.scripts.errors().empty());

    std::string err;
    CHECK_FALSE(room.scripts.run("forge.set(1, 'NoSuch', 'x', 1)", &err));
    CHECK(err.find("NoSuch") != std::string::npos);
}

TEST_CASE("scripts reach the game's variables and screens, and hear messages sent to everyone") {
    PoolScope pool;
    Room room;
    struct Bridge final : GameBridge {
        std::map<std::string, f64, std::less<>> numbers;
        std::map<std::string, std::string, std::less<>> texts;
        std::vector<std::string> screens;
        f64 var(std::string_view name, std::string* text) override {
            if (auto t = texts.find(name); t != texts.end() && text) *text = t->second;
            auto n = numbers.find(name);
            return n == numbers.end() ? 0 : n->second;
        }
        void set_var(std::string_view name, f64 v) override { numbers[std::string(name)] = v; }
        void set_text(std::string_view name, std::string_view t) override { texts[std::string(name)] = std::string(t); }
        void screen(std::string_view what, std::string_view name) override { screens.push_back(std::string(what) + " " + std::string(name)); }
    } bridge;
    room.scripts.set_user(kGameBridge, &bridge);
    bridge.numbers["inv.coins"] = 3;
    REQUIRE(room.scripts.load("shop", R"(
        local S = {}
        function S.on_message(self, name, value, from)
            if name == "купил" then
                forge.game.add_var("inv.coins", -2)
                forge.game.set_var("shop.last", "кирка")
                if forge.game.var("inv.coins") < 2 then forge.ui.show("Пусто") end
                forge.ui.hide("Лавка")
            end
        end
        return S
    )"));
    room.thing(4, 5, "shop");
    room.thing(8, 5, "shop");
    room.run(2);
    room.scripts.send(0, "купил"); // a button's message: to every listening thing
    room.run(2);
    CHECK(bridge.numbers["inv.coins"] == -1); // both things heard it
    CHECK(bridge.texts["shop.last"] == "кирка");
    CHECK(bridge.screens == std::vector<std::string>{"show Пусто", "hide Лавка", "show Пусто", "hide Лавка"});
    CHECK(room.scripts.errors().empty());
}

TEST_CASE("every engine function has a description for the node palette") {
    PoolScope pool;
    Room room;
    const ScriptApi& api = room.scripts.api();
    CHECK(api.all().size() >= 25);
    for (const ApiFunction& f : api.all()) {
        CAPTURE(f.name);
        CHECK(f.fn != nullptr);
        CHECK(!f.title_ru.empty());
        CHECK(!f.category.empty());
        for (const ApiParam& p : f.params) CHECK(!p.title_ru.empty());
    }
    const ApiFunction* wait = api.find("wait");
    REQUIRE(wait);
    CHECK(wait->kind == ApiKind::Latent);
    CHECK(wait->params[0].default_lua == "1");
}
