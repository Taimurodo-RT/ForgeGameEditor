// Part 6а of docs/models/сцены-модель.md («Сцена и актёры»): a scene's stage puts shapes, pictures of «Ресурсы» and
// characters of «Векторная» on it, each where and as big as it says; its scene(p) is recorded as a clip, and an actor
// plays its character's clips in the scene's time (play waits, loop goes round, stop holds), the scene's own
// transitions of the actor's parts over them by rule 5. The marks of the actors' clips are the scene's at their
// moments, on every round of a loop. A frame of the scene is the background, the pictures and the actors drawn alone
// in their poses, laid over each other in the stage's order.

#include "forge/assets/image.h"
#include "forge/pixel/bake.h"
#include "forge/pixel/doc.h"
#include "forge/pixel/draw.h"
#include "forge/pixel/motion.h"
#include "forge/pixel/rig.h"
#include "forge/pixel/scene.h"
#include "forge/pixel/script.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <ostream> // doctest's INFO prints a string_view through it; MSVC needs the whole class
#include <string>
#include <vector>

using namespace forge;
using namespace forge::pixel;

namespace {

// The hedgehog: a head and a nose in a body, feet below. walk bobs the head and marks a step halfway, wave turns the
// nose, stretch is as long as p.len.
const char* kHedgehog = R"(return {
  params = { height = 4, len = 1 },
  clips = {
    walk = function(p)
      part("head").y(-p.height, 0.25)
      emit("шаг")
      part("head").y(0, 0.25)
    end,
    wave = function(p)
      part("nose").rot(30, 0.5)
    end,
    stretch = function(p)
      wait(p.len)
    end,
  },
  draw = function(p, rng)
    canvas(64, 64)
    group("body", function()
      circle("head", { x = 32, y = 28, r = 14, fill = "#c09060" })
      rect("nose", { x = 30, y = 26, w = 4, h = 3, fill = "#a06040" })
    end)
    rect("feet", { x = 20, y = 50, w = 24, h = 8, fill = "#604020" })
  end,
}
)";

// The beetle: hop lifts the shell and swaps to the flat drawing on the way down.
const char* kBeetle = R"(return {
  clips = {
    hop = function(p)
      part("shell").y(-6, 0.3)
      swap("flat", 0.2)
      part("shell").y(0, 0.3)
    end,
  },
  draw = function(p, rng)
    canvas(48, 32)
    ellipse("shell", { x = 24, y = 18, rx = 18, ry = 12, fill = "#30a050" })
    circle("eye", { x = 36, y = 14, r = 3, fill = "#ffffff" })
  end,
  drawings = {
    flat = function(p, rng)
      canvas(48, 32)
      ellipse("shell", { x = 24, y = 22, rx = 20, ry = 8, fill = "#208040" })
      circle("eye", { x = 38, y = 20, r = 3, fill = "#ffffff" })
    end,
  },
}
)";

// «Ресурсы» in memory: the two characters, a 40 × 30 picture and a few broken files.
struct Files {
    std::map<std::string, std::vector<u8>> bytes;

    void put(const std::string& path, std::string_view text) { bytes[path].assign(text.begin(), text.end()); }

    SceneFiles scene() {
        SceneFiles f;
        f.read = [this](const std::string& path, std::vector<u8>& out, std::string& why) {
            const auto it = bytes.find(path);
            if (it == bytes.end()) return why = "нет файла «" + path + "»", false;
            out = it->second;
            return true;
        };
        f.svg = [](const std::string& guid, std::string&, std::string& why) { return why = "нет SVG " + guid, false; };
        return f;
    }
};

assets::CookedTexture forest() {
    assets::CookedTexture t;
    t.width = 40;
    t.height = 30;
    t.rgba8.resize(40 * 30 * 4);
    for (u32 y = 0; y < 30; ++y)
        for (u32 x = 0; x < 40; ++x) {
            u8* p = &t.rgba8[(y * 40 + x) * 4];
            p[0] = static_cast<u8>(40 + x * 4), p[1] = static_cast<u8>(120 + y * 3), p[2] = 60, p[3] = 255;
        }
    return t;
}

void project(Files& f) {
    PixelDoc h;
    h.script = "Ёжик.luau";
    h.frame_w = h.frame_h = 64;
    f.put("персонажи/Ёжик.pixel.json", write_pixel_doc(h));
    f.put("персонажи/Ёжик.luau", kHedgehog);
    PixelDoc b;
    b.script = "Жук.luau";
    b.frame_w = 48;
    b.frame_h = 32;
    f.put("персонажи/Жук.pixel.json", write_pixel_doc(b));
    f.put("персонажи/Жук.luau", kBeetle);
    PixelDoc bad = h;
    bad.script = "Сломан.luau";
    f.put("персонажи/Сломан.pixel.json", write_pixel_doc(bad));
    f.put("персонажи/Сломан.luau", "return {\n  clips = { walk = function(p)\n    wait(1)\n  end },\n  draw = function(p, rng)\n    rect(\"a\", { w = 1 })\n  end,\n}\n");
    PixelDoc sc = h;
    sc.script = "Сцена.luau";
    f.put("персонажи/Сцена.pixel.json", write_pixel_doc(sc));
    f.put("персонажи/Сцена.luau", new_scene_code("Сцена"));
    std::vector<u8> png;
    REQUIRE(assets::encode_image(forest(), ".png", png));
    f.bytes["картинки/лес.png"] = png;
    f.put("картинки/лес.jpg", "не картинка");
}

// The scene's code: stage from line 3, scene from the line after the stage's end.
std::string scene_code(std::string_view stage, std::string_view scene) {
    return "return {\n  stage = function(p, rng)\n" + std::string(stage) + "\n  end,\n  scene = function(p)\n" + std::string(scene) + "\n  end,\n}\n";
}

// Lines 3–8; the scene's from 11.
const char* kStage = R"(    canvas(160, 120)
    background("#203040")
    picture("лес", "картинки/лес.png", { x = 8, y = 6 })
    actor("ёжик", "персонажи/Ёжик", { x = 40, y = 100, pivot = { 32, 60 } })
    actor("жук", "персонажи/Жук", { x = 120, y = 90, scale = 0.5, rot = 15, pivot = { 24, 16 } })
    path("дорога", { d = "M 40 100 L 120 100", fill = "none", stroke = "#ffffff", hidden = true }))";

bool has(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

void load(Scene& s, Files& files, std::string_view scene, std::string_view stage = kStage) {
    const bool ok = s.load(scene_code(stage, scene), {}, files.scene());
    INFO(s.error());
    REQUIRE(ok);
}

void record(Scene& s) {
    const bool ok = s.record();
    INFO(s.error());
    REQUIRE(ok);
}

const ActorPlay& play_of(const Recording& rec, std::string_view actor, ActorPlay::Kind kind, usize nth = 0) {
    for (const ActorPlay& p : rec.plays)
        if (p.actor == actor && p.kind == kind && nth-- == 0) return p;
    FAIL("no such play");
    return rec.plays.front();
}

} // namespace

TEST_CASE("Сцена: stage ставит фигуры, картинки и актёров") {
    Files files;
    project(files);
    Scene s;
    load(s, files, "    wait(1)");
    const Stage& st = s.stage();
    CHECK(st.width() == 160);
    CHECK(st.height() == 120);
    CHECK(st.background == "#203040");
    CHECK(st.background_line == 4);
    REQUIRE(st.items.size() == 3);
    const StageItem& pic = st.items[0];
    CHECK(pic.kind == StageItem::Kind::Picture);
    CHECK(pic.id == "лес");
    CHECK(pic.file == "картинки/лес.png");
    CHECK(pic.x == 8);
    CHECK(pic.y == 6);
    CHECK(pic.w == 40);
    CHECK(pic.h == 30);
    CHECK_FALSE(pic.sized);
    CHECK(pic.line == 5);
    const StageItem& hog = st.items[1];
    CHECK(hog.kind == StageItem::Kind::Actor);
    CHECK(hog.file == "персонажи/Ёжик");
    CHECK(hog.w == 64);
    CHECK(hog.h == 64);
    CHECK(hog.origin.x == 0);
    CHECK(hog.origin.y == 0);
    CHECK(hog.pivot.x == 32);
    CHECK(hog.pivot.y == 60);
    CHECK(hog.line == 6);
    const StageItem& bug = st.items[2];
    CHECK(bug.scale == 0.5);
    CHECK(bug.rot == 15);
    CHECK(bug.w == 48);
    CHECK(bug.h == 32);
    // The order of the calls: each item's group where it was put, the path after them.
    std::vector<std::string> ids;
    for (const DrawShape& d : st.drawing.shapes) ids.push_back(d.id);
    CHECK(ids == std::vector<std::string>{"лес", "ёжик", "жук", "дорога"});
    for (const StageItem& it : st.items) CHECK(st.drawing.shapes[static_cast<usize>(it.group)].id == it.id);

    // The stage's rig: the items are groups, the actors' pictures hosted in theirs with their ids under the actor's.
    const Rig& rig = s.rig();
    CHECK(rig.width() == 160);
    for (const char* id : {"лес", "ёжик", "жук", "дорога"}) CHECK(rig.part(id) >= 0);
    REQUIRE(s.actor_rig("ёжик"));
    CHECK(rig.hosted("ёжик") == s.actor_rig("ёжик"));
    CHECK(rig.hosted("лес") == nullptr);
    CHECK(s.actor_rig("лес") == nullptr);
    CHECK(&rig.holder("ёжик/head") == s.actor_rig("ёжик"));
    CHECK(rig.holder("ёжик/head").part("ёжик/head") >= 0);
    CHECK(rig.holder("ёжик/нет").part("ёжик/нет") < 0);
    CHECK(&rig.holder("жук/shell") == s.actor_rig("жук"));
    CHECK(rig.holder("жук/shell").drawings() == std::vector<std::string>{"flat"});
    // A picture turns round its middle, an actor round the point of the scene its pivot stands at.
    const vector::Point pp = rig.pivot(rig.part("лес")), ap = rig.pivot(rig.part("ёжик"));
    CHECK(pp.x == 28);
    CHECK(pp.y == 21);
    CHECK(ap.x == 40);
    CHECK(ap.y == 100);
    // The actor's parts turn round its character's pivots.
    const SceneCharacter* c = s.character("ёжик");
    REQUIRE(c);
    const Rig& ar = *s.actor_rig("ёжик");
    for (const char* id : {"body", "head", "nose", "feet"}) {
        const vector::Point a = ar.pivot(ar.part(std::string("ёжик/") + id)), b = c->rig.pivot(c->rig.part(id));
        CHECK(a.x == b.x);
        CHECK(a.y == b.y);
    }
    CHECK(s.character("лес") == nullptr);
    const std::vector<std::string>& read = s.files();
    for (const char* f : {"картинки/лес.png", "персонажи/Ёжик.pixel.json", "персонажи/Ёжик.luau", "персонажи/Жук.pixel.json", "персонажи/Жук.luau"})
        CHECK(std::find(read.begin(), read.end(), f) != read.end());

    const std::string svg = stage_svg(st);
    CHECK(has(svg, "transform=\"translate(8 6)\""));
    CHECK(has(svg, "transform=\"matrix("));
    CHECK(has(svg, "fill=\"none\""));

    // Without canvas the scene is 1920 × 1080; a picture of the size it is given; a background as #rrggbb.
    Scene big;
    load(big, files, "    wait(1)", "    background(\"#234\")\n    picture(\"лес\", \"картинки/лес.png\", { x = 10, y = 20, w = 80, h = 60 })");
    CHECK(big.stage().width() == kSceneWidth);
    CHECK(big.stage().height() == kSceneHeight);
    CHECK(big.stage().background == "#223344");
    REQUIRE(big.stage().items.size() == 1);
    CHECK(big.stage().items[0].sized);
    CHECK(big.stage().items[0].w == 80);
    CHECK(big.stage().items[0].h == 60);
    Scene bare;
    load(bare, files, "    wait(1)", "    rect(\"a\", { x = 0, y = 0, w = 4, h = 4 })");
    CHECK(bare.stage().background.empty());
}

TEST_CASE("Сцена: матрица актёра") {
    StageItem a;
    a.kind = StageItem::Kind::Actor;
    a.x = 100, a.y = 50, a.pivot = {32, 60}, a.scale = 2, a.sx = 1.5, a.rot = 90;
    const Affine m = actor_matrix(a);
    // The pivot stands at x, y; a step right in the picture is 3 down on the scene (turned clockwise, scaled 2 × 1.5),
    // a step down is 2 left.
    const vector::Point p = m.apply({32, 60}), r = m.apply({33, 60}), d = m.apply({32, 61});
    CHECK(p.x == doctest::Approx(100));
    CHECK(p.y == doctest::Approx(50));
    CHECK(r.x == doctest::Approx(100));
    CHECK(r.y == doctest::Approx(53));
    CHECK(d.x == doctest::Approx(98));
    CHECK(d.y == doctest::Approx(50));
    StageItem plain;
    plain.x = 7, plain.y = -3;
    CHECK(actor_matrix(plain) == translation(7, -3));
}

TEST_CASE("Сцена: ошибки stage — со строкой вызова") {
    Files files;
    project(files);
    struct Case {
        const char* stage;
        const char* says;
    };
    const Case cases[] = {
        {"    actor(\"ёжик\", \"персонажи/Нет\")", "нет файла «персонажи/Нет.pixel.json»"},
        {"    picture(\"лес\", \"картинки/нет.png\")", "нет файла «картинки/нет.png»"},
        {"    picture(\"лес\", \"картинки/лес.jpg\")", "не PNG"},
        {"    actor(\"ёжик\", \"персонажи/Сломан\")", "actor «ёжик»: персонаж «персонажи/Сломан»: код «Сломан.luau»: строка 6"},
        {"    actor(\"ёжик\", \"персонажи/Сцена\")", "сцена, а не персонаж"},
        {"    actor(\"ё/ж\", \"персонажи/Ёжик\")", "буквы, цифры"},
        {"    actor(\"ёжик\", \"персонажи/Ёжик\", { z = 1 })", "поля «z» нет"},
        {"    picture(\"лес\", \"картинки/лес.png\", { w = 10 })", "w и h — вместе"},
        {"    actor(\"ёжик\", \"персонажи/Ёжик\", { params = { len = \"x\" } })", "params — таблица чисел"},
        {"    group(\"g\", function()\n      picture(\"лес\", \"картинки/лес.png\")\n    end)", "не в группу"},
        {"    rect(\"лес\", { x = 0, y = 0, w = 2, h = 2 })\n    picture(\"лес\", \"картинки/лес.png\")", "уже есть на сцене"},
        {"    background(\"#000\")\n    background(\"#111\")", "background — один раз"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.stage);
        Scene s;
        CHECK_FALSE(s.load(scene_code(c.stage, "    wait(1)"), {}, files.scene()));
        CHECK_FALSE(s.loaded());
        INFO(s.error());
        CHECK(has(s.error(), c.says));
        // The call that failed: the last line of the stage.
        const u32 lines = static_cast<u32>(std::count(c.stage, c.stage + std::strlen(c.stage), '\n'));
        CHECK(s.error_line() == 3 + lines - (has(c.stage, "group") ? 1 : 0));
    }
    // A broken character's code says its line.
    Scene s;
    CHECK_FALSE(s.load(scene_code("    actor(\"ёжик\", \"персонажи/Сломан\")", "    wait(1)"), {}, files.scene()));
    CHECK(has(s.error(), "строка 6"));
    // A scene's module has no clips, draw or pose; nor is a character's code a scene.
    CHECK_FALSE(s.load("return {\n  stage = function(p, rng) end,\n  scene = function(p) end,\n  clips = {},\n}\n", {}, files.scene()));
    CHECK(has(s.error(), "у сцены нет clips"));
    CHECK(s.error_line() == 4);
    CHECK_FALSE(s.load(kHedgehog, {}, files.scene()));
    CHECK(has(s.error(), "это не сцена"));
    CHECK_FALSE(s.load("return {\n  stage = function(p, rng) end,\n}\n", {}, files.scene()));
    CHECK(has(s.error(), "scene = function(p)"));
}

TEST_CASE("Сцена: клипы актёров во времени сцены") {
    Files files;
    project(files);
    Scene s;
    load(s, files, R"(    actor("ёжик"):play("walk")
    actor("ёжик"):play("walk", { speed = 2 })
    actor("ёжик"):loop("wave")
    spawn(actor("жук"):play("hop"))
    wait(1)
    actor("ёжик"):stop()
    wait(0.5))");
    CHECK(s.recording() == nullptr);
    record(s);
    const Recording& rec = *s.recording();
    CHECK(rec.length == doctest::Approx(2.25));
    REQUIRE(rec.plays.size() == 5);
    // play waits its clip's length / speed; loop, spawn and stop do not.
    const ActorPlay& walk = play_of(rec, "ёжик", ActorPlay::Kind::Play);
    CHECK(walk.clip == "walk");
    CHECK(walk.start == 0);
    CHECK(walk.length == doctest::Approx(0.5));
    CHECK(walk.clip_length == doctest::Approx(0.5));
    CHECK(walk.site.site >= 0);
    const ActorPlay& fast = play_of(rec, "ёжик", ActorPlay::Kind::Play, 1);
    CHECK(fast.start == doctest::Approx(0.5));
    CHECK(fast.speed == 2);
    CHECK(fast.length == doctest::Approx(0.25));
    const ActorPlay& wave = play_of(rec, "ёжик", ActorPlay::Kind::Loop);
    CHECK(wave.start == doctest::Approx(0.75));
    const ActorPlay& hop = play_of(rec, "жук", ActorPlay::Kind::Play);
    CHECK(hop.start == doctest::Approx(0.75));
    CHECK(hop.length == doctest::Approx(0.8));
    const ActorPlay& stop = play_of(rec, "ёжик", ActorPlay::Kind::Stop);
    CHECK(stop.start == doctest::Approx(1.75));
    CHECK(stop.clip.empty());

    // Which clip at which moment of it.
    struct At {
        const char* actor;
        f64 t;
        const ActorPlay* play;
        f64 time;
    };
    const At at[] = {
        {"ёжик", 0, &walk, 0},       {"ёжик", 0.25, &walk, 0.25}, {"ёжик", 0.6, &fast, 0.2},  {"ёжик", 1.05, &wave, 0.3},
        {"ёжик", 1.6, &wave, 0.35},  {"ёжик", 2.0, &wave, 0},     {"ёжик", 9, &wave, 0},       {"жук", 0.5, nullptr, 0},
        {"жук", 0.95, &hop, 0.2},    {"жук", 2.0, &hop, 0.8},     {"нет", 1, nullptr, 0},
    };
    for (const At& a : at) {
        CAPTURE(a.actor);
        CAPTURE(a.t);
        const ActorMoment m = actor_moment(rec, a.actor, a.t);
        CHECK(m.play == a.play);
        CHECK(m.time == doctest::Approx(a.time));
    }
    // A stop holds the moment the one before it showed at its start, a stop after a stop that one's.
    Recording two = rec;
    ActorPlay again = stop;
    again.start = 2.1;
    again.order += 100;
    two.plays.push_back(again);
    const ActorMoment held = actor_moment(two, "ёжик", 2.2);
    CHECK(held.play == &two.plays[2]);
    CHECK(held.time == doctest::Approx(0));

    // A clip's parameters: the stage's over the character's, a play's over both; the clip recorded with them.
    Scene p;
    load(p, files, R"(    actor("ёжик"):play("stretch", { params = { len = 2 } })
    actor("ёжик"):play("stretch")
    actor("ёж2"):play("stretch"))",
         "    actor(\"ёжик\", \"персонажи/Ёжик\")\n    actor(\"ёж2\", \"персонажи/Ёжик\", { params = { len = 0.5 } })");
    record(p);
    CHECK(p.recording()->length == doctest::Approx(3.5));
    CHECK(play_of(*p.recording(), "ёжик", ActorPlay::Kind::Play).length == doctest::Approx(2));
    CHECK(play_of(*p.recording(), "ёжик", ActorPlay::Kind::Play).params.at("len") == 2);
    CHECK(play_of(*p.recording(), "ёжик", ActorPlay::Kind::Play, 1).length == doctest::Approx(1));
    CHECK(play_of(*p.recording(), "ёж2", ActorPlay::Kind::Play).length == doctest::Approx(0.5));
}

TEST_CASE("Сцена: что сцене нельзя — со строкой") {
    Files files;
    project(files);
    struct Case {
        const char* scene;
        const char* says;
    };
    const Case cases[] = {
        {"    part(\"ёжик/head\"):along(\"дорога\", 1)", "у части актёра «ёжик/head» нет along"},
        {"    part(\"ёжик/head\"):attach(\"лес\")", "у части актёра «ёжик/head» нет attach"},
        {"    part(\"ёжик/head\"):snap_to(\"лес\", 0.2)", "у части актёра «ёжик/head» нет snap_to"},
        {"    copies(\"ёжик\", 2, function(i, c) end)", "копии актёра — не в этой версии"},
        {"    copies(\"ёжик/head\", 2, function(i, c) end)", "у части актёра «ёжик/head» нет copies"},
        {"    part(\"лес\"):along(\"ёжик/head\", 1)", "часть актёра, а здесь нужна часть сцены"},
        {"    part(\"лес\"):attach(\"ёжик/head\")", "часть актёра, а здесь нужна часть сцены"},
        {"    part(\"ёжик/head\"):mask(\"дорога\")", "из другого рисунка"},
        {"    part(\"дорога\"):morph(\"ёжик/head\", 0.2)", "из другого рисунка"},
        {"    part(\"лес\").fill(\"#ff0000\", 0.2)", "у картинки «лес» нет «fill»"},
        {"    part(\"лес\").tint(\"#ff0000\", 0.2)", "у картинки «лес» нет «tint»"},
        {"    swap(\"flat\", 0.2)", "swap — у персонажа"},
        {"    start_from(\"walk\")", "start_from — у клипа персонажа"},
        {"    actor(\"ёжик\"):part(\"нет\")", "у актёра «ёжик» нет части «нет»"},
        {"    actor(\"нет\"):play(\"walk\")", "на сцене нет актёра «нет»"},
        {"    actor(\"лес\"):play(\"walk\")", "на сцене нет актёра «лес»"},
        {"    actor(\"ёжик\"):play(\"walk\", { speed = 0 })", "speed — число больше 0"},
        {"    actor(\"ёжик\"):play(\"walk\", { blend = 0.2 })", "blend — в части 6г"},
        {"    actor(\"ёжик\"):play(\"walk\", { pace = 1 })", "поля «pace» нет"},
        {"    actor(\"ёжик\"):play(\"walk\", { params = { len = \"x\" } })", "params — таблица чисел"},
        {"    actor(\"ёжик\"):play()", "первым — имя клипа"},
        {"    actor(\"ёжик\"):stop(1)", "stop() — без аргументов"},
        {"    actor(\"жук\"):play(\"walk\")", "у персонажа «персонажи/Жук» нет клипа «walk» (есть «hop»)"},
        {"    camera()", "camera — в части 6б"},
        {"    transition(\"сдвиг\", 1)", "transition — в части 6б"},
        {"    music(\"тема\")", "music — в части 6в"},
        {"    talk(\"разговор\")", "talk — в части 6в"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.scene);
        Scene s;
        load(s, files, c.scene);
        CHECK_FALSE(s.record());
        CHECK(s.recording() == nullptr);
        INFO(s.error());
        CHECK(has(s.error(), c.says));
        CHECK(s.error_line() == 11);
    }
    // The whole actor is a part of the stage: it goes along the stage's path and is attached to its picture.
    Scene s;
    load(s, files, "    part(\"ёжик\"):along(\"дорога\", 1)\n    part(\"жук\"):attach(\"лес\")\n    part(\"лес\").x(10, 1)");
    record(s);
    CHECK(s.frame(1));
}

TEST_CASE("Сцена: переход сцены поверх позы клипа (правило 5)") {
    PartPose clip;
    clip.x = 1, clip.y = 2, clip.rot = 10, clip.skew_y = 3, clip.scale = 2, clip.sy = 0.5, clip.opacity = 0.5, clip.dash = 1;
    clip.has_fill = true, clip.fill = {10, 20, 30, 255}, clip.fill_k = 1;
    clip.draw_to = 0.5;
    clip.has_tint = true, clip.tint = {1, 2, 3, 255};
    clip.flash = 0.25;
    clip.blur = 2;
    clip.mask = "ёжик/m";
    PartPose scene;
    scene.x = 2, scene.y = -2, scene.rot = 5, scene.skew_x = 4, scene.skew_y = 1, scene.scale = 1.5, scene.sx = 2, scene.sy = 3;
    scene.opacity = 0.5, scene.dash = 2;
    PartPose a = clip;
    over_part(a, scene);
    CHECK(a.x == 3);
    CHECK(a.y == 0);
    CHECK(a.rot == 15);
    CHECK(a.skew_x == 4);
    CHECK(a.skew_y == 4);
    CHECK(a.dash == 3);
    CHECK(a.scale == 3);
    CHECK(a.sx == 2);
    CHECK(a.sy == 1.5);
    CHECK(a.opacity == 0.25);
    CHECK(a.visible);
    // What the scene did not move from rest is the clip's.
    CHECK(a.has_fill);
    CHECK(a.fill == Rgba8{10, 20, 30, 255});
    CHECK(a.draw_from == 0);
    CHECK(a.draw_to == 0.5);
    CHECK(a.tint == Rgba8{1, 2, 3, 255});
    CHECK(a.flash == 0.25);
    CHECK(a.blur == 2);
    CHECK(a.mask == "ёжик/m");
    // What it moved is its own; seen when both say so.
    PartPose looks;
    looks.has_fill = true, looks.fill = {200, 0, 0, 255}, looks.fill_k = 0.5;
    looks.has_stroke = true, looks.stroke = {0, 200, 0, 255}, looks.stroke_k = 0.25;
    looks.has_stroke_width = true, looks.stroke_width = 3, looks.stroke_width_k = 1;
    looks.draw_from = 0.2;
    looks.has_tint = true, looks.tint = {9, 9, 9, 255}, looks.tint_k = 0.75;
    looks.flash = 0.5;
    looks.has_flash_color = true, looks.flash_color = {255, 0, 0, 255};
    looks.blur = 1;
    looks.has_shadow = true, looks.shadow = {0, 0, 0, 128}, looks.shadow_x = 2;
    looks.mask = "ёжик/n", looks.mask_invert = true;
    looks.visible = false;
    PartPose b = clip;
    over_part(b, looks);
    CHECK(b.fill == Rgba8{200, 0, 0, 255});
    CHECK(b.fill_k == 0.5);
    CHECK(b.has_stroke);
    CHECK(b.stroke_k == 0.25);
    CHECK(b.has_stroke_width);
    CHECK(b.stroke_width == 3);
    CHECK(b.draw_from == doctest::Approx(0.2));
    CHECK(b.draw_to == 1);
    CHECK(b.tint == Rgba8{9, 9, 9, 255});
    CHECK(b.tint_k == 0.75);
    CHECK(b.flash == 0.5);
    CHECK(b.has_flash_color);
    CHECK(b.blur == 1);
    CHECK(b.has_shadow);
    CHECK(b.shadow_x == 2);
    CHECK(b.mask == "ёжик/n");
    CHECK(b.mask_invert);
    CHECK_FALSE(b.visible);
    // A colour at k = 0 is the scene at rest: the clip's stays.
    PartPose rest;
    rest.has_fill = true, rest.fill = {1, 1, 1, 255}, rest.fill_k = 0;
    PartPose c = clip;
    over_part(c, rest);
    CHECK(c.fill == Rgba8{10, 20, 30, 255});
}

TEST_CASE("Сцена: часть актёра — клип в его миг и переход сцены поверх") {
    Files files;
    project(files);
    Scene s;
    load(s, files, R"(    actor("ёжик"):loop("walk")
    part("ёжик/head").x(5, 0)
    part("ёжик/head"):to({ rot = 20, sx = 2 }, 0.5)
    part("ёжик").x(10, 0)
    part("ёжик").tint("#ff0000", 0)
    actor("ёжик"):part("nose").opacity(0.5, 0)
    wait(1))");
    record(s);
    const f64 t = 0.6;
    REQUIRE(s.frame(t));
    const ActorMoment m = actor_moment(*s.recording(), "ёжик", t);
    REQUIRE(m.play);
    CHECK(m.time == doctest::Approx(0.1));
    // The same character posed by its clip at that moment, the scene's transition over it.
    SceneCharacter c;
    std::string why;
    REQUIRE(load_character(files.scene(), "персонажи/Ёжик", c, why));
    PoseScript code;
    REQUIRE(code.load(c.code));
    Pose pose;
    REQUIRE(code.pose(c.rig, m.time, "walk", c.doc.params, c.doc.seed, pose));
    PartPose head;
    head.x = 5, head.rot = 20, head.sx = 2;
    over_part(pose.parts["head"], head);
    pose.parts["nose"].opacity *= 0.5;
    REQUIRE(c.rig.pose(pose, why));
    const Rig& ar = *s.actor_rig("ёжик");
    for (const char* id : {"head", "nose", "body"}) {
        CAPTURE(id);
        const Affine want = c.rig.matrix(c.rig.part(id)), got = ar.matrix(ar.part(std::string("ёжик/") + id));
        CHECK(got.a == doctest::Approx(want.a));
        CHECK(got.b == doctest::Approx(want.b));
        CHECK(got.c == doctest::Approx(want.c));
        CHECK(got.d == doctest::Approx(want.d));
        CHECK(got.e == doctest::Approx(want.e));
        CHECK(got.f == doctest::Approx(want.f));
    }
    // The whole actor: moved as its group of the stage, round its pivot; tinted as its picture.
    const Rig& rig = s.rig();
    const Affine g = rig.matrix(rig.part("ёжик")), want = translation(10, 0) * actor_matrix(s.stage().items[1]);
    CHECK(g.e == doctest::Approx(want.e));
    CHECK(g.f == doctest::Approx(want.f));
    const assets::CookedTexture px = s.draw(160, 120);
    REQUIRE(px.width == 160);
    // The head above the nose, (32, 20) of the hedgehog's picture: at 40 − 32 + 10 + 32, 100 − 60 + 20 of the scene, red.
    const u8* head_px = &px.rgba8[(60 * 160 + 50) * 4];
    CHECK(head_px[0] == 0xc0);
    CHECK(head_px[1] == 0);
    CHECK(head_px[2] == 0);
    CHECK(head_px[3] == 255);
}

TEST_CASE("Сцена: метки клипов — метки сцены в их мигах") {
    Files files;
    project(files);
    Scene s;
    load(s, files, R"(    emit("начало")
    actor("ёжик"):play("walk")
    actor("ёжик"):loop("walk", { speed = 2 })
    wait(0.7))");
    record(s);
    std::vector<Mark> marks;
    REQUIRE(s.marks(marks));
    REQUIRE(marks.size() == 5);
    CHECK(marks[0].name == "начало");
    CHECK(marks[0].actor.empty());
    const f64 times[] = {0, 0.25, 0.625, 0.875, 1.125};
    for (usize i = 0; i < 5; ++i) {
        CAPTURE(i);
        CHECK(marks[i].time == doctest::Approx(times[i]));
        if (i) {
            CHECK(marks[i].name == "шаг");
            CHECK(marks[i].actor == "ёжик");
        }
    }
    // A clip cut by the actor's next one gives no marks past it.
    Scene cut;
    load(cut, files, R"(    spawn(actor("ёжик"):play("walk"))
    wait(0.1)
    actor("ёжик"):play("wave"))");
    record(cut);
    REQUIRE(cut.marks(marks));
    CHECK(marks.empty());
    // Before recording there are none.
    Scene fresh;
    load(fresh, files, "    wait(1)");
    REQUIRE(fresh.marks(marks));
    CHECK(marks.empty());
}

namespace {

// dst over by src, straight RGBA.
void over(assets::CookedTexture& dst, const assets::CookedTexture& src) {
    REQUIRE(dst.rgba8.size() == src.rgba8.size());
    for (usize i = 0; i < dst.rgba8.size(); i += 4) {
        const f64 sa = src.rgba8[i + 3] / 255.0, da = dst.rgba8[i + 3] / 255.0, oa = sa + da * (1 - sa);
        for (int k = 0; k < 3; ++k) {
            const f64 c = oa > 0 ? (src.rgba8[i + k] * sa + dst.rgba8[i + k] * da * (1 - sa)) / oa : 0;
            dst.rgba8[i + k] = static_cast<u8>(std::lround(c));
        }
        dst.rgba8[i + 3] = static_cast<u8>(std::lround(oa * 255));
    }
}

} // namespace

TEST_CASE("Сцена: кадр — фон, картинка и актёры, нарисованные по отдельности") {
    Files files;
    project(files);
    Scene s;
    load(s, files, R"(    actor("ёжик"):loop("walk")
    actor("жук"):play("hop")
    wait(0.4))");
    record(s);
    CHECK(s.recording()->length == doctest::Approx(1.2));
    // The characters drawn alone, as their own tabs draw them.
    struct Alone {
        const char* id;
        const char* file;
        SceneCharacter c;
        PoseScript code;
    };
    Alone alone[2] = {{"ёжик", "персонажи/Ёжик", {}, {}}, {"жук", "персонажи/Жук", {}, {}}};
    for (Alone& a : alone) {
        std::string why;
        REQUIRE(load_character(files.scene(), a.file, a.c, why));
        REQUIRE(a.code.load(a.c.code));
    }
    for (const f64 t : {0.0, 0.1, 0.35, 0.45, 1.0, 1.2}) {
        CAPTURE(t);
        REQUIRE(s.frame(t));
        const assets::CookedTexture got = s.draw(160, 120);
        assets::CookedTexture want;
        want.width = 160;
        want.height = 120;
        want.rgba8.resize(160 * 120 * 4);
        for (usize i = 0; i < want.rgba8.size(); i += 4) want.rgba8[i] = 0x20, want.rgba8[i + 1] = 0x30, want.rgba8[i + 2] = 0x40, want.rgba8[i + 3] = 255;
        const assets::CookedTexture pic = forest();
        for (u32 y = 0; y < 30; ++y)
            for (u32 x = 0; x < 40; ++x)
                for (int k = 0; k < 4; ++k) want.rgba8[((y + 6) * 160 + x + 8) * 4 + k] = pic.rgba8[(y * 40 + x) * 4 + k];
        const assets::CookedTexture still = want;
        for (usize i = 0; i < 2; ++i) {
            Alone& a = alone[i];
            const ActorMoment m = actor_moment(*s.recording(), a.id, t);
            REQUIRE(m.play);
            Pose pose;
            REQUIRE(a.code.pose(a.c.rig, m.time, m.play->clip, a.c.doc.params, a.c.doc.seed, pose));
            std::string why;
            REQUIRE(a.c.rig.pose(pose, why));
            over(want, a.c.rig.draw(160, 120, actor_matrix(s.stage().items[i + 1])));
        }
        int worst = 0;
        usize moved = 0;
        for (usize i = 0; i < want.rgba8.size(); ++i) {
            worst = std::max(worst, std::abs(int(got.rgba8[i]) - int(want.rgba8[i])));
            moved += got.rgba8[i] != still.rgba8[i];
        }
        // Within 2 of 255 a channel: ThorVG blends each shape over the scene in 8 bits, the test the actor drawn
        // alone over it.
        CHECK(worst <= 2);
        CHECK(moved > 500); // the actors are there
    }
}

TEST_CASE("Сцена: файл сцены и новая сцена") {
    SceneDoc d;
    d.script = "Вступление.luau";
    d.seed = 7;
    d.params = {{"speed", 1.5}};
    d.fps = 30;
    d.ranged = true;
    d.from = 0.5;
    d.to = 4;
    d.lengths.waits["встреча"] = 0.6;
    d.lengths.durations["оборот"] = 0.8;
    const std::string text = write_scene_doc(d);
    SceneDoc back;
    std::string why;
    REQUIRE(read_scene_doc(text, back, why));
    CHECK(back == d);
    CHECK(write_scene_doc(back) == text);
    SceneDoc plain;
    plain.script = "Сцена.luau";
    const std::string bare = write_scene_doc(plain);
    CHECK_FALSE(has(bare, "range"));
    CHECK_FALSE(has(bare, "waits"));
    CHECK_FALSE(has(bare, "durations"));
    REQUIRE(read_scene_doc(bare, back, why));
    CHECK(back == plain);
    const char* bad[][2] = {
        {"[]", "не объект"},
        {"{\"fps\": 0}", "fps"},
        {"{\"fps\": 300}", "fps"},
        {"{\"seed\": -1}", "seed"},
        {"{\"script\": 1}", "script"},
        {"{\"params\": {\"a\": \"b\"}}", "params.a"},
        {"{\"range\": [2, 1]}", "range"},
        {"{\"waits\": {\"a\": -1}}", "waits.a"},
        {"{\"durations\": []}", "durations"},
        {"{", "это не JSON"},
    };
    for (const auto& [json, says] : bad) {
        CAPTURE(json);
        SceneDoc keep = d;
        CHECK_FALSE(read_scene_doc(json, keep, why));
        CHECK(has(why, says));
        CHECK(keep == d);
    }
    // A new scene: 1920 × 1080, its background, a second long.
    Files files;
    Scene s;
    REQUIRE(s.load(new_scene_code("Сцена"), {}, files.scene()));
    CHECK(s.stage().width() == 1920);
    CHECK(s.stage().background == "#203040");
    CHECK(s.stage().items.empty());
    record(s);
    CHECK(s.recording()->length == doctest::Approx(1));
    CHECK(s.frame(0.5));
    const assets::CookedTexture px = s.draw(192, 108);
    REQUIRE(px.width == 192);
    CHECK(px.rgba8[0] == 0x20);
    CHECK(px.rgba8[1] == 0x30);
    CHECK(px.rgba8[2] == 0x40);
    CHECK(has(new_scene_code("Встреча"), "-- «Встреча»"));
}

TEST_CASE("Сцена: ids актёра под его именем") {
    const std::string svg = R"svg(<?xml version="1.0"?>
<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="10" height="10" viewBox="0 0 10 10">
  <defs><linearGradient id="g"><stop offset="0" stop-color="#fff"/></linearGradient></defs>
  <!-- id="c" url(#c) -->
  <rect id="a" width="4" height="4" fill="url(#g)" style="stroke: url('#g')"/>
  <use href="#a" xlink:href="#a" x="5"/>
  <g id='b' data-id="x"><circle cx="5" cy="5" r="1"/></g>
</svg>
)svg";
    const std::string out = prefix_svg_ids(svg, "ёжик");
    for (const char* want : {"id=\"ёжик/g\"", "fill=\"url(#ёжик/g)\"", "stroke: url('#ёжик/g')", "href=\"#ёжик/a\"", "xlink:href=\"#ёжик/a\"",
                             "id='ёжик/b'", "data-id=\"x\"", "<!-- id=\"c\" url(#c) -->", "viewBox=\"0 0 10 10\">\n<g id=\"ёжик\">", "</g>\n</svg>"})
        CHECK_MESSAGE(has(out, want), want);
    Rig rig;
    std::string why;
    REQUIRE_MESSAGE(rig.load(out, why), why);
    CHECK(rig.part("ёжик") >= 0);
    CHECK(rig.part("ёжик/a") >= 0);
    CHECK(rig.part("ёжик/b") >= 0);
    CHECK(rig.part("a") < 0);
    CHECK(rig.parts()[static_cast<usize>(rig.part("ёжик/a"))].parent == rig.part("ёжик"));

    Pose p;
    p.parts["head"].x = 1;
    p.parts["head"].mask = "eyes";
    p.parts["arm"].carry_by = "body";
    p.morphs["mouth"] = {"mouth_o", 0.5};
    p.order.push_back({"nose", OrderStep::Kind::Above, "head"});
    p.copies.push_back({"искра[1]", "искра", 1});
    p.drawing = "flat";
    p.swap = true;
    p.swap_from = "angry";
    p.swap_k = 0.25;
    const Pose q = prefix_pose(p, "ёжик");
    CHECK(q.parts.size() == 2);
    CHECK(q.parts.at("ёжик/head").x == 1);
    CHECK(q.parts.at("ёжик/head").mask == "ёжик/eyes");
    CHECK(q.parts.at("ёжик/head").carry_by.empty());
    CHECK(q.parts.at("ёжик/arm").carry_by == "ёжик/body");
    CHECK(q.morphs.at("ёжик/mouth").target == "ёжик/mouth_o");
    CHECK(q.morphs.at("ёжик/mouth").k == 0.5);
    CHECK(q.order[0] == OrderStep{"ёжик/nose", OrderStep::Kind::Above, "ёжик/head"});
    CHECK(q.copies[0] == PartCopy{"ёжик/искра[1]", "ёжик/искра", 1});
    CHECK(q.drawing == "flat");
    CHECK(q.swap);
    CHECK(q.swap_from == "angry");
    CHECK(q.swap_k == 0.25);
}
