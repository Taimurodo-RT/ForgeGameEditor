#include "forge/core/jobs.h"
#include "forge/script/host.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
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

TEST_CASE("scripts change the parts of a screen's live SVG picture and ease a share of time") {
    PoolScope pool;
    Room room;
    struct Bridge final : GameBridge {
        std::vector<std::string> said;
        f64 var(std::string_view, std::string*) override { return 0; }
        void set_var(std::string_view, f64) override {}
        void set_text(std::string_view, std::string_view) override {}
        void screen(std::string_view, std::string_view) override {}
        bool svg(std::string_view what, std::string_view screen, std::string_view layer, std::string_view part, f64 x, f64 y,
                 std::string_view text, std::string* why) override {
            if (layer == "Нет") {
                *why = "на экране «" + std::string(screen) + "» нет слоя «Нет»";
                return false;
            }
            char numbers[64];
            std::snprintf(numbers, sizeof(numbers), "%g %g", x, y);
            said.push_back(std::string(what) + " " + std::string(screen) + "/" + std::string(layer) + "/" + std::string(part) + " " + numbers +
                           (text.empty() ? "" : " " + std::string(text)));
            return true;
        }
    } bridge;
    room.scripts.set_user(kGameBridge, &bridge);
    REQUIRE(room.scripts.load("face", R"(
        local S = {}
        function S.on_start(self)
            forge.ui.svg_move("hud", "Лицо", "глаза", 2, -1)
            forge.ui.svg_turn("hud", "Лицо", "глаза", 45)
            forge.ui.svg_color("hud", "Лицо", "глаза", "#00aa00")
            forge.ui.svg_fade("hud", "Лицо", "щёки", 0.25)
            forge.ui.svg_morph("hud", "Лицо", "рот", "улыбка", forge.ease("smooth", 0.5))
            forge.ui.svg_morph("hud", "Лицо", "рот", "улыбка", forge.ease("linear", 2))
            forge.ui.svg_reset("hud", "Лицо")
        end
        return S
    )"));
    room.thing(4, 5, "face");
    room.run(2);
    CHECK(room.scripts.errors().empty());
    CHECK(bridge.said == std::vector<std::string>{"move hud/Лицо/глаза 2 -1", "turn hud/Лицо/глаза 45 0", "color hud/Лицо/глаза 0 0 #00aa00",
                                                  "fade hud/Лицо/щёки 0.25 0", "morph hud/Лицо/рот 0.5 0 улыбка", "morph hud/Лицо/рот 1 0 улыбка",
                                                  "reset hud/Лицо/ 0 0"});
    // What the game cannot do is the script's error, at its line; so is an easing it has not.
    REQUIRE(room.scripts.load("wrong", R"(
        local S = {}
        function S.on_start(self)
            forge.ui.svg_turn("hud", "Нет", "глаза", 45)
        end
        return S
    )"));
    REQUIRE(room.scripts.load("easing", R"(
        local S = {}
        function S.on_start(self)
            forge.ui.svg_fade("hud", "Лицо", "щёки", forge.ease("rubber", 0.5))
        end
        return S
    )"));
    room.thing(6, 5, "wrong");
    room.thing(7, 5, "easing");
    room.run(2);
    REQUIRE(room.scripts.errors().size() == 2);
    CHECK(room.scripts.errors()[0].message.find("ui.svg_turn: на экране «hud» нет слоя «Нет»") != std::string::npos);
    CHECK(room.scripts.errors()[0].line == 4);
    CHECK(room.scripts.errors()[1].message.find("ease: «rubber» — нет такого") != std::string::npos);
    const ApiFunction* morph = room.scripts.api().find("ui.svg_morph");
    REQUIRE(morph);
    CHECK(morph->kind == ApiKind::Action);
    CHECK(morph->params.size() == 5);
    CHECK(morph->params[3].title_ru == std::string("В фигуру (id)"));
}

TEST_CASE("scripts play a character's clip on a screen's picture: the loop, the speed, the blend and the parameters, and what they cannot be") {
    PoolScope pool;
    Room room;
    struct Bridge final : GameBridge {
        std::vector<std::string> said;
        std::vector<f64> blends; // part 5е: the blend of each play, the rest as svg says it
        std::vector<std::map<std::string, f64>> params; // part 5ж: the parameters of each play
        bool svg_play(std::string_view screen, std::string_view layer, std::string_view clip, const SvgPlay& how, std::string* why) override {
            blends.push_back(how.blend);
            params.push_back(how.params);
            return GameBridge::svg_play(screen, layer, clip, how, why);
        }
        f64 var(std::string_view, std::string*) override { return 0; }
        void set_var(std::string_view, f64) override {}
        void set_text(std::string_view, std::string_view) override {}
        void screen(std::string_view, std::string_view) override {}
        bool svg(std::string_view what, std::string_view screen, std::string_view layer, std::string_view part, f64 x, f64 y,
                 std::string_view, std::string* why) override {
            if (part == "fly") {
                *why = "в «Скакун.motion.json» нет клипа «fly»: есть «idle», «jump»";
                return false;
            }
            char numbers[64];
            std::snprintf(numbers, sizeof(numbers), "%g %g", x, y);
            said.push_back(std::string(what) + " " + std::string(screen) + "/" + std::string(layer) + "/" + std::string(part) + " " + numbers);
            return true;
        }
    } bridge;
    room.scripts.set_user(kGameBridge, &bridge);
    // x: the loop (1, 0, below 0 for the clip's own), y: the speed.
    REQUIRE(room.scripts.load("clips", R"(
        local S = {}
        function S.on_start(self)
            forge.ui.svg_play("hud", "Скакун", "idle")
            forge.ui.svg_play("hud", "Скакун", "jump", true)
            forge.ui.svg_play("hud", "Скакун", "jump", false)
            forge.ui.svg_play("hud", "Скакун", "jump", nil)
            forge.ui.svg_play("hud", "Скакун", "idle", { speed = 2 })
            forge.ui.svg_play("hud", "Скакун", "idle", { loop = true, speed = 0.5 })
            forge.ui.svg_play("hud", "Скакун", "idle", { loop = false })
            forge.ui.svg_play("hud", "Скакун", "idle", {})
            forge.ui.svg_play("hud", "Скакун", "run", { blend = 0.2 })
            forge.ui.svg_play("hud", "Скакун", "run", true, 0.25)
            forge.ui.svg_play("hud", "Скакун", "run", { loop = false, speed = 2, blend = 0 })
            forge.ui.svg_play("hud", "Скакун", "run", nil, 0.5)
            forge.ui.svg_play("hud", "Скакун", "run", false, nil)
            forge.ui.svg_play("hud", "Скакун", "jump", { params = { height = 8 } })
            forge.ui.svg_play("hud", "Скакун", "jump", { blend = 0.1, params = { height = -3, count = 2.5 } })
            forge.ui.svg_play("hud", "Скакун", "jump", { params = {} })
            forge.ui.svg_stop("hud", "Скакун")
        end
        return S
    )"));
    room.thing(4, 5, "clips");
    room.run(2);
    CHECK(room.scripts.errors().empty());
    CHECK(bridge.said == std::vector<std::string>{"play hud/Скакун/idle -1 1", "play hud/Скакун/jump 1 1", "play hud/Скакун/jump 0 1",
                                                  "play hud/Скакун/jump -1 1", "play hud/Скакун/idle -1 2", "play hud/Скакун/idle 1 0.5",
                                                  "play hud/Скакун/idle 0 1", "play hud/Скакун/idle -1 1", "play hud/Скакун/run -1 1",
                                                  "play hud/Скакун/run 1 1", "play hud/Скакун/run 0 2", "play hud/Скакун/run -1 1",
                                                  "play hud/Скакун/run 0 1", "play hud/Скакун/jump -1 1", "play hud/Скакун/jump -1 1",
                                                  "play hud/Скакун/jump -1 1", "stop hud/Скакун/ 0 0"});
    CHECK(bridge.blends == std::vector<f64>{0, 0, 0, 0, 0, 0, 0, 0, 0.2, 0.25, 0, 0.5, 0, 0, 0.1, 0});
    REQUIRE(bridge.params.size() == 16);
    for (usize i = 0; i < 13; ++i) CHECK(bridge.params[i].empty());
    CHECK(bridge.params[13] == std::map<std::string, f64>{{"height", 8}});
    CHECK(bridge.params[14] == std::map<std::string, f64>{{"count", 2.5}, {"height", -3}});
    CHECK(bridge.params[15].empty());
    // Each wrong call is the script's error at its line, and the game is not asked: one script each, as a script stops
    // at its first error.
    const char* wrong[][2] = {
        {R"(forge.ui.svg_play("hud", "Скакун", "jump", "да"))", "ui.svg_play: 4-е значение (повторять): нужно true или false"},
        {R"(forge.ui.svg_play("hud", "Скакун", "jump", { loop = 1 }))", "ui.svg_play: loop: нужно true или false"},
        {R"(forge.ui.svg_play("hud", "Скакун", "jump", { speed = "быстро" }))", "ui.svg_play: speed: нужно число больше 0"},
        {R"(forge.ui.svg_play("hud", "Скакун", "jump", { speed = 0 }))", "ui.svg_play: speed: нужно число больше 0"},
        {R"(forge.ui.svg_play("hud", "Скакун", "jump", { speed = -1 }))", "ui.svg_play: speed: нужно число больше 0"},
        {R"(forge.ui.svg_play("hud", "Скакун", "jump", { speed = 0/0 }))", "ui.svg_play: speed: нужно число больше 0"},
        {R"(forge.ui.svg_play("hud", "Скакун", "jump", { speed = 1/0 }))", "ui.svg_play: speed: нужно число больше 0"},
        {R"(forge.ui.svg_play("hud", "Скакун", "jump", { fast = true }))", "ui.svg_play: в таблице только loop, speed, blend и params, а не «fast»"},
        {R"(forge.ui.svg_play("hud", "Скакун", "jump", { true }))", "ui.svg_play: в таблице только loop, speed, blend и params, а не «number»"},
        {R"(forge.ui.svg_play("hud", "Скакун", "jump", { params = 8 }))", "ui.svg_play: params: нужна таблица { имя = число }"},
        {R"(forge.ui.svg_play("hud", "Скакун", "jump", { params = { height = "высоко" } }))", "ui.svg_play: params.height: нужно число"},
        {R"(forge.ui.svg_play("hud", "Скакун", "jump", { params = { height = 0/0 } }))", "ui.svg_play: params.height: нужно число"},
        {R"(forge.ui.svg_play("hud", "Скакун", "jump", { params = { height = -1/0 } }))", "ui.svg_play: params.height: нужно число"},
        {R"(forge.ui.svg_play("hud", "Скакун", "jump", { params = { 8 } }))", "ui.svg_play: params: имена параметров — строки, а не number"},
        {R"(forge.ui.svg_play("hud", "Скакун", "jump", { blend = -0.1 }))", "ui.svg_play: blend: нужно число секунд, 0 или больше"},
        {R"(forge.ui.svg_play("hud", "Скакун", "jump", { blend = "долго" }))", "ui.svg_play: blend: нужно число секунд, 0 или больше"},
        {R"(forge.ui.svg_play("hud", "Скакун", "jump", { blend = 0/0 }))", "ui.svg_play: blend: нужно число секунд, 0 или больше"},
        {R"(forge.ui.svg_play("hud", "Скакун", "jump", { blend = 1/0 }))", "ui.svg_play: blend: нужно число секунд, 0 или больше"},
        {R"(forge.ui.svg_play("hud", "Скакун", "jump", true, -1))", "ui.svg_play: 5-е значение (плавно за, с): нужно число секунд, 0 или больше"},
        {R"(forge.ui.svg_play("hud", "Скакун", "jump", true, "долго"))", "ui.svg_play: 5-е значение (плавно за, с): нужно число секунд, 0 или больше"},
        {R"(forge.ui.svg_play("hud", "Скакун", "jump", { loop = true }, 1/0))", "ui.svg_play: 5-е значение (плавно за, с): нужно число секунд, 0 или больше"},
        {R"(forge.ui.svg_play("hud", "Скакун", "fly"))", "ui.svg_play: в «Скакун.motion.json» нет клипа «fly»: есть «idle», «jump»"},
        {R"(forge.ui.svg_play("hud", "Скакун"))", "missing argument #3 to 'ui.svg_play'"},
        {R"(forge.ui.svg_stop("hud"))", "missing argument #2 to 'ui.svg_stop'"},
    };
    bridge.said.clear();
    bridge.blends.clear();
    for (usize i = 0; i < std::size(wrong); ++i) {
        const std::string name = "wrong" + std::to_string(i);
        REQUIRE(room.scripts.load(name, "local S = {}\nfunction S.on_start(self)\n  " + std::string(wrong[i][0]) + "\nend\nreturn S\n"));
        room.thing(6 + static_cast<i32>(i), 5, name.c_str());
    }
    room.run(2);
    REQUIRE(room.scripts.errors().size() == std::size(wrong));
    for (usize i = 0; i < std::size(wrong); ++i) {
        const ScriptError* e = nullptr;
        for (const ScriptError& x : room.scripts.errors())
            if (x.script == "wrong" + std::to_string(i)) e = &x;
        REQUIRE_MESSAGE(e, wrong[i][0]);
        CHECK_MESSAGE(e->message.find(wrong[i][1]) != std::string::npos, e->message);
        CHECK(e->line == 3);
    }
    CHECK(bridge.said.empty());
    CHECK(bridge.blends == std::vector<f64>{0}); // «fly» only: the game says it has no such clip
    const ApiFunction* play = room.scripts.api().find("ui.svg_play");
    REQUIRE(play);
    CHECK(play->kind == ApiKind::Action);
    REQUIRE(play->params.size() == 5);
    CHECK(play->params[3].title_ru == std::string("Повторять"));
    CHECK(play->params[4].title_ru == std::string("Плавно за, с"));
    CHECK(play->params[4].default_lua == std::string("0"));
    const ApiFunction* stop = room.scripts.api().find("ui.svg_stop");
    REQUIRE(stop);
    CHECK(stop->params.size() == 2);
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
